using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace RAReplay.Client
{
    public sealed class ReplayArchive
    {
        private const int HeaderBytes = 96;
        private const int StreamHeaderBytes = 32;
        private const long MaximumStreamBytes = 1024L * 1024 * 1024;
        // 容器 v3 的清单段与帧流段都至少是一个块：20 字节块头加至少 1 字节载荷。
        private const uint MinimumBlockSegmentBytes = ReplayBlocks.HeaderBytes + 1;
        // 帧记录 flags：bit0 checksum 有效，bit1 编队完整快照（版本 3 起），bit2 记有游戏速度（版本 4 起）。
        private const uint GroupSnapshotFlag = 2;
        private const uint GameSpeedFlag = 4;
        private const uint MaximumGameSpeed = 6;
        public string Path { get; private set; }
        public ReplayLaunchManifest Manifest { get; private set; }
        // 容器版本：2 为清单与帧流原样存放；3 为清单与帧流按独立块压缩（解压后的字节与 v2 相同）。
        public uint ContainerVersion { get; private set; }
        // 帧流段的磁盘字节数，用于变更检测与浏览器条目键。完成文件为头中记录的长度；
        // 未完成录像为可恢复部分的磁盘字节数：v2 为可恢复帧流长度（含 32 字节帧流头），v3 为有效块前缀的磁盘字节数。
        public long StreamBytes { get; private set; }
        public byte[] GameExeHash { get; private set; }
        public uint StreamVersion { get; private set; }
        // 帧流头 gameMode（0 战役、5 遭遇战为单机），供对局信息区分单机与联机；帧流头不完整时为空。
        public uint? StreamGameMode { get; private set; }
        // 容器头 flags==0 的未完成录像（录制异常中止遗留）；是否未完成只看 flags，与文件后缀无关。
        public bool IsUnfinalized { get; private set; }
        // 未完成录像从帧 0 起可恢复的连续完整有效帧数；完成文件为 0。
        public uint RecoveredFrameCount { get; private set; }
        public bool HasPlayerInputEffects { get { return StreamVersion >= 2; } }
        public bool HasRecordedGroups { get { return StreamVersion >= 3; } }
        public bool HasGameSpeedRecords { get { return StreamVersion >= 4; } }
        private byte[] metadataHash;
        private byte[] contentHash;
        private long streamOffset;
        // 容器头 metadataBytes：清单段的磁盘字节数（v3 为清单块的块头加载荷）。
        private uint metadataBytes;
        // 未完成录像可恢复帧之后的逻辑帧流位置（含 32 字节帧流头）；v2 与 StreamBytes 相等，完成文件不使用。
        private long recoveredLogicalBytes;

        // 回放目录中可发现的文件：完成文件 *.yrrep 与异常中止遗留的 *.yrrep.partial。
        // Windows 通配符可能按短文件名匹配到其他扩展名，故枚举后再以后缀精确过滤；
        // 后缀只用于发现文件，是否未完成仍以容器头 flags 为准。
        public static IEnumerable<string> EnumerateReplayFiles(string directory)
        {
            foreach (string file in Directory.EnumerateFiles(directory, "*.yrrep*", SearchOption.TopDirectoryOnly))
            {
                if (file.EndsWith(".yrrep", StringComparison.OrdinalIgnoreCase) ||
                    file.EndsWith(".yrrep.partial", StringComparison.OrdinalIgnoreCase))
                    yield return file;
            }
        }

        // 打开回放目录下全部可识别的回放，按最后写入时间倒序；打不开的文件（损坏、无法恢复、正被录制独占）静默跳过。
        // 会读取文件，调用方应在非界面线程执行。目录不存在时返回空列表。
        public static List<ReplayArchive> OpenAll(string directory)
        {
            List<ReplayArchive> result = new List<ReplayArchive>();
            if (!Directory.Exists(directory)) return result;
            foreach (string file in EnumerateReplayFiles(directory).OrderByDescending(File.GetLastWriteTimeUtc))
            {
                try { result.Add(Open(file)); }
                catch (Exception) { /* 损坏、无法恢复或正被录制独占的文件不能进入播放列表。 */ }
            }
            return result;
        }

        // 读取容器头与清单。完成文件只取帧流头的版本与 gameMode（v3 只解码读取帧流头所需的块，正常只有第一块）；
        // 未完成录像在逻辑帧流上扫描可恢复帧（v3 先按块恢复规则在首个无效块之前截止）。
        public static ReplayArchive Open(string path)
        {
            using (FileStream stream = OpenSequential(path))
            using (BinaryReader reader = new BinaryReader(stream, Encoding.UTF8))
            {
                byte[] magic = Exact(reader, 8);
                if (Encoding.ASCII.GetString(magic) != "RAREPL2\0")
                    throw new InvalidDataException("不支持的回放文件。");
                uint containerVersion = reader.ReadUInt32();
                if ((containerVersion != 2 && containerVersion != 3) || reader.ReadUInt32() != HeaderBytes)
                    throw new InvalidDataException("不支持的回放文件。");
                bool blocks = containerVersion == 3;
                uint metadataBytes = reader.ReadUInt32();
                uint flags = reader.ReadUInt32();
                ulong streamBytes = reader.ReadUInt64();
                byte[] gameExeHash = Exact(reader, 32);
                byte[] hash = Exact(reader, 32);
                // v2 的 metadataBytes、streamBytes 是未压缩长度；v3 是磁盘字节数，两段各至少一个块（21 字节起）。
                // flags 只允许 1（完成）与 0（未完成），其他值一律拒绝。
                if (metadataBytes < (blocks ? MinimumBlockSegmentBytes : 1u) ||
                    metadataBytes > ReplayLaunchManifest.MaximumBytes || flags > 1)
                    throw new InvalidDataException("回放长度或完成标志无效。");
                long offset = HeaderBytes + (long)metadataBytes;
                bool unfinalized = flags == 0;
                if (unfinalized)
                {
                    // 未完成录像的头在创建时写入，长度与内容哈希保持为零，帧流长度由扫描确定。
                    if (streamBytes != 0 || !IsAllZero(hash))
                        throw new InvalidDataException("未完成录像的长度与内容哈希必须为零。");
                    if (blocks)
                    {
                        // v3 的帧流头在解码后的逻辑帧流中，是否完整由块扫描决定；这里只要求清单段完整存在。
                        if (stream.Length < offset)
                            throw new InvalidDataException("未完成录像的开局清单段不完整。");
                    }
                    else if (stream.Length - offset < StreamHeaderBytes)
                        throw new InvalidDataException("未完成录像缺少完整的帧流头。");
                    // 候选帧流（v3 为候选帧流段）长度为文件长度 - 96 - metadataBytes；超过 1 GiB 直接拒绝，不截断后再扫描。
                    if (stream.Length - offset > MaximumStreamBytes)
                        throw new InvalidDataException("未完成录像的帧流超过 1 GiB 上限。");
                }
                else if (streamBytes < (blocks ? MinimumBlockSegmentBytes : (ulong)StreamHeaderBytes) ||
                    streamBytes > MaximumStreamBytes || stream.Length != offset + (long)streamBytes)
                    throw new InvalidDataException("回放长度或完成标志无效。");
                byte[] metadata = blocks ? ReplayBlocks.ReadManifest(stream, metadataBytes) : Exact(reader, (int)metadataBytes);
                long recoveredBytes = (long)streamBytes;
                long recoveredLogical = (long)streamBytes;
                uint recoveredFrames = 0;
                uint streamVersion;
                uint? streamGameMode = null;
                if (unfinalized)
                {
                    // v2 的逻辑帧流就是 metadata 之后的全部字节；v3 逐块解码候选帧流段，在首个无效块之前截止。
                    long candidateBytes = stream.Length - offset;
                    ReplayBlockStream blockStream = blocks ? new ReplayBlockStream(stream, offset, candidateBytes, true) : null;
                    Stream logical = blockStream ?? (Stream)new FileSegmentStream(stream, offset, candidateBytes);
                    FrameReader frames = new FrameReader(logical, long.MaxValue);
                    if (!frames.TryEnsure(StreamHeaderBytes))
                        throw new InvalidDataException("未完成录像缺少完整的帧流头。");
                    uint gameMode;
                    streamVersion = ReadStreamHeader(frames, out gameMode);
                    streamGameMode = gameMode;
                    long recoveredEnd;
                    bool reachedEnd;
                    recoveredFrames = ScanFrames(frames, streamVersion, gameMode, false, out recoveredEnd, out reachedEnd);
                    if (recoveredFrames == 0)
                        throw new InvalidDataException("未完成录像没有可恢复的完整帧。");
                    recoveredLogical = recoveredEnd;
                    recoveredBytes = recoveredEnd;
                    if (blockStream != null)
                    {
                        // 帧扫描可能停在有效块中途：继续扫描其后的块，得到有效块前缀，并对有效块累计执行 1 GiB 上限。
                        blockStream.Drain();
                        recoveredBytes = blockStream.ValidDiskBytes;
                    }
                }
                else if (blocks)
                {
                    // 完成文件从逻辑帧流读取帧流头前 16 字节（正常只解码第一块，第一块不足 16 字节时继续读后续块），
                    // 不遍历全部块；完整性由 ValidatePayload 的 SHA-256、逐块解码与逐帧校验保证。
                    byte[] head = new byte[16];
                    int headBytes = ReadUpTo(new ReplayBlockStream(stream, offset, (long)streamBytes, false), head);
                    if (headBytes < 8 || ReadUInt32(head, 0) != 0x32465052)
                        throw new InvalidDataException("回放帧流头无效。");
                    streamVersion = ReadUInt32(head, 4);
                    if (!IsSupportedStreamVersion(streamVersion))
                        throw new InvalidDataException("不支持的回放帧流版本。");
                    // eventBytes 与 gameMode 只读不校验；逻辑帧流不足 16 字节时 gameMode 留空，由 ValidatePayload 拒绝。
                    if (headBytes >= 16) streamGameMode = ReadUInt32(head, 12);
                }
                else
                {
                    // 完成文件只取版本与 gameMode；完整性由 ValidatePayload 的 SHA-256 与逐帧校验保证。
                    if (reader.ReadUInt32() != 0x32465052)
                        throw new InvalidDataException("回放帧流头无效。");
                    streamVersion = reader.ReadUInt32();
                    if (!IsSupportedStreamVersion(streamVersion))
                        throw new InvalidDataException("不支持的回放帧流版本。");
                    // eventBytes 与 gameMode 只读不校验；剩余字节不足时 gameMode 留空，不新增失败路径。
                    if (stream.Length - stream.Position >= 8)
                    {
                        reader.ReadUInt32(); // eventBytes
                        streamGameMode = reader.ReadUInt32();
                    }
                }
                return new ReplayArchive
                {
                    Path = System.IO.Path.GetFullPath(path),
                    Manifest = ReplayLaunchManifest.Deserialize(metadata),
                    ContainerVersion = containerVersion,
                    StreamBytes = recoveredBytes,
                    GameExeHash = gameExeHash,
                    StreamVersion = streamVersion,
                    StreamGameMode = streamGameMode,
                    IsUnfinalized = unfinalized,
                    RecoveredFrameCount = recoveredFrames,
                    metadataHash = ReplayLaunchManifest.Hash(metadata),
                    contentHash = hash,
                    streamOffset = offset,
                    metadataBytes = metadataBytes,
                    recoveredLogicalBytes = recoveredLogical
                };
            }
        }

        // 播放前流式核对：完成文件核对文件偏移 96 起全部磁盘字节的 SHA-256，再在逻辑帧流上严格逐帧校验
        // （v3 同时要求每块有效、块链正好结束在 streamBytes 处）；列表浏览只解析头与清单。
        public void ValidatePayload()
        {
            ValidatePayload(null);
        }

        // unitEnd 不为空时，严格逐帧校验依次报告帧流头与每条帧记录结束处的逻辑位置（压缩旧回放据此切分写入单元，
        // 帧记录边界因此只来自这一套严格扫描）。校验失败照常抛出，已报告的位置不再有意义。
        internal void ValidatePayload(Action<long> unitEnd)
        {
            ReplayArchive current = Open(Path);
            if (IsUnfinalized)
            {
                // 未完成录像没有内容哈希，不做 SHA-256；以重新扫描的可恢复部分（v2 为帧流长度，v3 为有效块前缀）、
                // 可恢复帧数与 metadata 不变证明文件未被改动。
                if (!current.IsUnfinalized || current.ContainerVersion != ContainerVersion ||
                    current.StreamBytes != StreamBytes || current.recoveredLogicalBytes != recoveredLogicalBytes ||
                    current.RecoveredFrameCount != RecoveredFrameCount ||
                    !ReplayLaunchManifest.EqualsHash(current.metadataHash, metadataHash))
                    throw new InvalidDataException("回放文件在校验前发生变化。");
                ValidateFrames(unitEnd);
                return;
            }
            if (current.IsUnfinalized || current.ContainerVersion != ContainerVersion || current.StreamBytes != StreamBytes ||
                !ReplayLaunchManifest.EqualsHash(current.contentHash, contentHash))
                throw new InvalidDataException("回放文件在校验前发生变化。");
            using (FileStream stream = OpenSequential(Path))
            using (SHA256 sha = SHA256.Create())
            {
                // 用头中的 metadataBytes（清单段磁盘字节数）核对长度；v3 的清单段是压缩块，不等于清单重新序列化的长度。
                if (stream.Length != HeaderBytes + (long)current.metadataBytes + StreamBytes)
                    throw new InvalidDataException("回放文件在校验期间发生变化。");
                stream.Position = HeaderBytes;
                long remaining = stream.Length - HeaderBytes;
                byte[] buffer = new byte[64 * 1024];
                while (remaining > 0)
                {
                    int read = stream.Read(buffer, 0, (int)Math.Min(buffer.Length, remaining));
                    if (read == 0) throw new EndOfStreamException("回放数据被截断。");
                    sha.TransformBlock(buffer, 0, read, buffer, 0);
                    remaining -= read;
                }
                sha.TransformFinalBlock(new byte[0], 0, 0);
                if (!ReplayLaunchManifest.EqualsHash(sha.Hash, contentHash))
                    throw new InvalidDataException("回放数据 SHA-256 校验失败。");
            }
            ValidateFrames(unitEnd);
        }

        public bool MatchesCurrentHeader()
        {
            ReplayArchive current = Open(Path);
            return current.IsUnfinalized == IsUnfinalized &&
                current.ContainerVersion == ContainerVersion &&
                current.RecoveredFrameCount == RecoveredFrameCount &&
                current.StreamBytes == StreamBytes &&
                ReplayLaunchManifest.EqualsHash(current.contentHash, contentHash) &&
                ReplayLaunchManifest.EqualsHash(current.metadataHash, metadataHash) &&
                ReplayLaunchManifest.EqualsHash(current.GameExeHash, GameExeHash);
        }

        // 帧数：未完成录像直接用扫描得到的可恢复帧数；完成文件需要扫描帧流（会读取整个帧流，应在后台线程调用）。
        // 完成文件的帧流没有完整通过扫描时返回 null（数字不可靠，不显示）；v3 任何块无效同样返回 null。
        // 文件已变化返回 null；帧流头无效按异常抛出。
        public uint? CountFrames()
        {
            if (IsUnfinalized) return RecoveredFrameCount;
            using (FileStream stream = OpenSequential(Path))
            {
                if (stream.Length != streamOffset + StreamBytes) return null;
                Stream logical = OpenLogicalStream(stream);
                ReplayBlockStream blocks = logical as ReplayBlockStream;
                try
                {
                    FrameReader reader = new FrameReader(logical, long.MaxValue);
                    uint gameMode;
                    if (ReadStreamHeader(reader, out gameMode) != StreamVersion) return null;
                    long validEnd;
                    bool reachedEnd;
                    uint frames = ScanFrames(reader, StreamVersion, gameMode, false, out validEnd, out reachedEnd);
                    return reachedEnd ? (uint?)frames : null;
                }
                catch (InvalidDataException) when (blocks != null && blocks.Failure != null) { return null; }
            }
        }

        // 在逻辑帧流上严格逐帧校验；完成文件与未完成录像的可恢复部分共用。
        // 完成文件校验到逻辑帧流末尾；未完成录像只读 [streamOffset, streamOffset + StreamBytes)，并在可恢复帧之后停止。
        // unitEnd 见 ValidatePayload(Action<long>)。
        private void ValidateFrames(Action<long> unitEnd)
        {
            using (FileStream stream = OpenSequential(Path))
            {
                long end = streamOffset + StreamBytes;
                // 完成文件必须恰好到文件尾；未完成录像可恢复部分之后的内容在校验范围外。
                if (IsUnfinalized ? stream.Length < end : stream.Length != end)
                    throw new InvalidDataException("回放文件在校验期间发生变化。");
                FrameReader reader = new FrameReader(OpenLogicalStream(stream),
                    IsUnfinalized ? recoveredLogicalBytes : long.MaxValue);
                uint gameMode;
                uint streamVersion = ReadStreamHeader(reader, out gameMode);
                if (streamVersion != StreamVersion) throw new InvalidDataException("回放帧流头无效。");
                if (unitEnd != null) unitEnd(reader.Position);
                long validEnd;
                bool reachedEnd;
                uint frames = ScanFrames(reader, streamVersion, gameMode, true, out validEnd, out reachedEnd, unitEnd);
                // V3 起首帧完整快照区分“没有记录”和“记录为空”，不能以零帧代替（V4、V5 沿用）。
                if (streamVersion >= 3 && frames == 0)
                    throw new InvalidDataException("回放缺少首帧编队快照。");
                if (IsUnfinalized && frames != RecoveredFrameCount)
                    throw new InvalidDataException("回放文件在校验期间发生变化。");
            }
        }

        // 逻辑帧流，帧流头位于逻辑偏移 0：v2 为文件 [streamOffset, streamOffset + StreamBytes) 的子流；
        // v3 为这段磁盘字节上的严格块解码流（未完成录像的这段是已确认的有效块前缀）。stream 为本回放文件的只读句柄。
        internal Stream OpenLogicalStream(FileStream stream)
        {
            if (ContainerVersion == 2) return new FileSegmentStream(stream, streamOffset, StreamBytes);
            return new ReplayBlockStream(stream, streamOffset, StreamBytes, false);
        }

        // 清单段的原始字节（解压后的 RPLM 开局清单）：v2 原样读出，v3 解码清单块。读到的内容不重新序列化。
        internal byte[] ReadMetadata()
        {
            using (FileStream stream = OpenSequential(Path))
            {
                stream.Position = HeaderBytes;
                if (ContainerVersion == 3) return ReplayBlocks.ReadManifest(stream, metadataBytes);
                byte[] metadata = new byte[metadataBytes];
                ReplayBlocks.ReadExact(stream, metadata, 0, metadata.Length);
                return metadata;
            }
        }

        // 完成文件的文件字节数（容器头、清单段与帧流段之和，Open 已核对等于文件长度）；未完成录像不使用。
        // 公开给宿主：压缩旧回放的确认框据此汇总候选回放的大小。
        public long FileBytes
        {
            get { return HeaderBytes + (long)metadataBytes + StreamBytes; }
        }

        // 帧流版本 1–5；版本 4 在版本 3 的帧头之后按 flags bit2 追加游戏速度。
        // 版本 5 的布局与校验规则与版本 4 完全相同，只表示单机帧流在录制时已排除飞机绘制对模拟的影响。
        private static bool IsSupportedStreamVersion(uint streamVersion)
        {
            return streamVersion >= 1 && streamVersion <= 5;
        }

        // 帧流头 gameMode 为 0（战役）或 5（遭遇战）即单机，其余均按联机处理。
        internal static bool IsSinglePlayerMode(uint gameMode)
        {
            return gameMode == 0 || gameMode == 5;
        }

        // 读取并核对位于逻辑偏移 0 的 32 字节帧流头，返回帧流版本；gameMode 供版本 4 起的游戏速度规则使用。
        private static uint ReadStreamHeader(FrameReader reader, out uint gameMode)
        {
            if (!reader.TryEnsure(StreamHeaderBytes)) throw new InvalidDataException("回放帧流头不完整。");
            uint magic = reader.ReadUInt32();
            uint streamVersion = reader.ReadUInt32();
            uint eventBytes = reader.ReadUInt32();
            if (magic != 0x32465052 || !IsSupportedStreamVersion(streamVersion) || eventBytes != 111)
                throw new InvalidDataException("回放帧流头无效。");
            gameMode = reader.ReadUInt32();
            reader.ReadUInt32(); // localHouseIndex
            if (reader.ReadUInt32() != 0) throw new InvalidDataException("回放起始帧无效。");
            reader.ReadUInt32(); // frameSendRate
            reader.ReadUInt32(); // randomSeed
            return streamVersion;
        }

        // 从当前位置（帧流头之后）逐条扫描帧记录，直到逻辑帧流结束（或 FrameReader 的上限）。
        // 严格模式遇到不完整或无效记录即抛异常；扫描模式在该记录之前截止。
        // 返回连续完整有效的帧数，validEnd 为最后一条有效记录之后的逻辑位置；reachedEnd 表示扫描恰好在帧边界读到结束。
        // frameEnd 不为空时，每读完一条有效记录就报告它结束处的逻辑位置。
        private static uint ScanFrames(FrameReader reader, uint streamVersion, uint gameMode, bool strict,
            out long validEnd, out bool reachedEnd, Action<long> frameEnd = null)
        {
            bool singlePlayer = IsSinglePlayerMode(gameMode);
            uint expected = 0;
            validEnd = reader.Position;
            reachedEnd = false;
            while (true)
            {
                if (reader.AtEnd())
                {
                    reachedEnd = true;
                    break;
                }
                string error;
                if (!TryReadFrame(reader, streamVersion, singlePlayer, expected, out error))
                {
                    if (strict) throw new InvalidDataException(error);
                    break;
                }
                expected++;
                validEnd = reader.Position;
                if (frameEnd != null) frameEnd(validEnd);
            }
            return expected;
        }

        // 读取一条完整有效的帧记录；记录不完整或字段无效返回 false（位置不再有意义）。
        // 版本 4 起（含版本 5）的记录在 24 字节帧头之后、事件之前，按 flags bit2 多出 4 字节游戏速度（0..6）：
        // 单机（gameMode 0/5）帧 0 必须带速度，后续帧可带可不带；联机任何帧都不得带速度。
        // 逻辑帧流可能是长度未知的只前进流（v3），载荷是否完整在读取或跳过时判定。
        private static bool TryReadFrame(FrameReader reader, uint streamVersion, bool singlePlayer,
            uint expected, out string error)
        {
            const string Truncated = "回放帧记录被截断。";
            int frameHeaderBytes = streamVersion >= 3 ? 24 : streamVersion == 2 ? 20 : 16;
            if (!reader.TryEnsure(frameHeaderBytes))
            {
                error = Truncated;
                return false;
            }
            uint frame = reader.ReadUInt32();
            reader.ReadUInt32(); // checksumBeforeDispatch
            uint count = reader.ReadUInt32();
            uint flags = reader.ReadUInt32();
            uint effectCount = streamVersion >= 2 ? reader.ReadUInt32() : 0;
            uint groupMemberCount = streamVersion >= 3 ? reader.ReadUInt32() : 0;
            uint allowedFlags = streamVersion >= 4 ? 7u : streamVersion == 3 ? 3u : 1u;
            bool hasGroupSnapshot = (flags & GroupSnapshotFlag) != 0;
            bool hasGameSpeed = streamVersion >= 4 && (flags & GameSpeedFlag) != 0;
            uint gameSpeed = 0;
            if (hasGameSpeed)
            {
                if (!reader.TryEnsure(4))
                {
                    error = Truncated;
                    return false;
                }
                gameSpeed = reader.ReadUInt32();
            }
            if (frame != expected || count > 16384 || (flags & ~allowedFlags) != 0 ||
                effectCount > 4096 || groupMemberCount > 16384 ||
                (!hasGroupSnapshot && groupMemberCount != 0) ||
                (streamVersion >= 3 && expected == 0 && !hasGroupSnapshot))
            {
                error = "回放帧顺序、事件数量或标志无效。";
                return false;
            }
            if (gameSpeed > MaximumGameSpeed ||
                (streamVersion >= 4 && singlePlayer && expected == 0 && !hasGameSpeed) ||
                (!singlePlayer && hasGameSpeed))
            {
                error = "回放游戏速度记录无效。";
                return false;
            }
            if (!reader.TrySkip(count * 111L))
            {
                error = Truncated;
                return false;
            }
            for (uint i = 0; i < effectCount; i++)
            {
                // 每个输入效果 40 字节：kind、animationTypeIndex、x、y、z、loopDelay、loopCount、flags、forceZAdjust、reverse。
                if (!reader.TryEnsure(40))
                {
                    error = Truncated;
                    return false;
                }
                uint kind = reader.ReadUInt32();
                reader.TrySkip(32);
                uint reverse = reader.ReadUInt32();
                if (kind != 1 || reverse > 1)
                {
                    error = "回放玩家输入效果字段无效。";
                    return false;
                }
            }
            return TryReadGroupMembers(reader, groupMemberCount, out error);
        }

        private static bool TryReadGroupMembers(FrameReader reader, uint count, out string error)
        {
            error = null;
            if (count == 0) return true;
            // 调用方已核对成员上限；集合仅在当前完整快照内去重。
            HashSet<uint> objectIds = new HashSet<uint>();
            uint previousGroup = 0;
            uint previousId = 0;
            for (uint i = 0; i < count; i++)
            {
                if (!reader.TryEnsure(12))
                {
                    error = "回放帧记录被截断。";
                    return false;
                }
                uint groupIndex = reader.ReadUInt32();
                uint objectId = reader.ReadUInt32();
                uint targetKind = reader.ReadUInt32();
                bool outOfOrder = i > 0 && (groupIndex < previousGroup ||
                    (groupIndex == previousGroup && objectId <= previousId));
                // 52 是目标引擎打包 NetID 的通用 Abstract 标记，并非具体单位类型。
                if (groupIndex > 9 || objectId == 0 || targetKind != 52 || outOfOrder ||
                    !objectIds.Add(objectId))
                {
                    error = "回放编队成员字段、排序或唯一性无效。";
                    return false;
                }
                previousGroup = groupIndex;
                previousId = objectId;
            }
            return true;
        }

        private static bool IsAllZero(byte[] data)
        {
            foreach (byte value in data)
                if (value != 0) return false;
            return true;
        }

        private static uint ReadUInt32(byte[] data, int offset)
        {
            return (uint)(data[offset] | data[offset + 1] << 8 | data[offset + 2] << 16 | data[offset + 3] << 24);
        }

        // 读到 buffer 填满或流结束，返回读到的字节数。
        private static int ReadUpTo(Stream stream, byte[] buffer)
        {
            int total = 0;
            while (total < buffer.Length)
            {
                int read = stream.Read(buffer, total, buffer.Length - total);
                if (read <= 0) break;
                total += read;
            }
            return total;
        }

        // 只读共享打开并提示顺序访问；文件共享与读取语义不变，只影响系统缓存的预读策略。
        private static FileStream OpenSequential(string path)
        {
            return new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 4096, FileOptions.SequentialScan);
        }

        // v2 的逻辑帧流：文件 [start, start + length) 的只读视图，逻辑位置 0 对应 start。
        // 读到段尾返回 0；文件比段短（读取期间被截断）抛 EndOfStreamException，不当作正常结束。
        private sealed class FileSegmentStream : Stream
        {
            private readonly Stream file;
            private readonly long start;
            private readonly long length;
            private long position;

            public FileSegmentStream(Stream file, long start, long length)
            {
                this.file = file;
                this.start = start;
                this.length = length;
                file.Position = start;
            }

            public override bool CanRead { get { return true; } }
            public override bool CanSeek { get { return true; } }
            public override bool CanWrite { get { return false; } }
            public override long Length { get { return length; } }

            public override long Position
            {
                get { return position; }
                set
                {
                    if (value < 0 || value > length) throw new ArgumentOutOfRangeException("value");
                    file.Position = start + value;
                    position = value;
                }
            }

            public override int Read(byte[] buffer, int offset, int count)
            {
                long available = length - position;
                if (available <= 0 || count == 0) return 0;
                int read = file.Read(buffer, offset, (int)Math.Min(count, available));
                if (read <= 0) throw new EndOfStreamException("回放文件被截断。");
                position += read;
                return read;
            }

            public override long Seek(long offset, SeekOrigin origin)
            {
                Position = origin == SeekOrigin.Begin ? offset : origin == SeekOrigin.Current ? position + offset : length + offset;
                return position;
            }

            public override void Flush() { }
            public override void SetLength(long value) { throw new NotSupportedException(); }
            public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
        }

        // 帧记录以 4 字节小字段为主，事件载荷只需要跳过；逐字段读取会在十万帧级别的文件上产生大量调用。
        // 这里在逻辑帧流上一次读入 64 KB 缓冲，字段从缓冲取出，载荷只移动缓冲内位置。
        // Position 为逻辑位置（帧流头位于 0）；读取不超过 limit。流结束（Read 返回 0）即逻辑帧流结束：
        // v2 子流在段尾结束，v3 块流在块链正常结束（严格模式）或首个无效块（恢复模式）处结束，块错误与 I/O 错误以异常抛出。
        private sealed class FrameReader
        {
            private const int BufferBytes = 64 * 1024;
            private readonly Stream stream;
            private readonly long limit;
            private readonly byte[] buffer = new byte[BufferBytes];
            private long bufferStart; // buffer[0] 对应的逻辑位置；已从流读出 bufferStart + length 字节
            private int index;
            private int length;
            private bool streamEnded;

            public FrameReader(Stream stream, long limit)
            {
                this.stream = stream;
                this.limit = limit;
            }

            public long Position { get { return bufferStart + index; } }

            // 保证缓冲内至少有 needed 个未读字节（needed 不超过缓冲容量）；流在此之前结束或到达 limit 返回 false。
            public bool TryEnsure(int needed)
            {
                if (length - index >= needed) return true;
                int remaining = length - index;
                if (remaining > 0 && index > 0) Buffer.BlockCopy(buffer, index, buffer, 0, remaining);
                bufferStart += index;
                index = 0;
                length = remaining;
                while (length < needed)
                {
                    if (streamEnded) return false;
                    long available = limit - (bufferStart + length);
                    int want = (int)Math.Min(buffer.Length - length, available);
                    if (want <= 0) return false;
                    int read = stream.Read(buffer, length, want);
                    if (read <= 0)
                    {
                        streamEnded = true;
                        return false;
                    }
                    length += read;
                }
                return true;
            }

            // 逻辑帧流（或 limit）在当前位置结束。
            public bool AtEnd()
            {
                return !TryEnsure(1);
            }

            // 调用方必须先以 TryEnsure 确认缓冲内至少有 4 个未读字节。
            public uint ReadUInt32()
            {
                if (length - index < 4) throw new InvalidOperationException("读取帧字段前未确认缓冲内的字节数。");
                uint value = (uint)(buffer[index] | buffer[index + 1] << 8 | buffer[index + 2] << 16 | buffer[index + 3] << 24);
                index += 4;
                return value;
            }

            // 跳过 count 字节；流在此之前结束或超过 limit 返回 false。
            public bool TrySkip(long count)
            {
                if (count <= length - index)
                {
                    index += (int)count;
                    return true;
                }
                if (limit - Position < count) return false;
                if (count <= BufferBytes)
                {
                    if (!TryEnsure((int)count)) return false;
                    index += (int)count;
                    return true;
                }
                // 超过缓冲的事件载荷：丢弃缓冲。可定位的子流（v2）直接移动位置，只前进的块流（v3）读出丢弃。
                long target = Position + count;
                bufferStart += length;
                index = 0;
                length = 0;
                if (stream.CanSeek)
                {
                    if (target > stream.Length) return false;
                    stream.Position = target;
                    bufferStart = target;
                    return true;
                }
                while (bufferStart < target)
                {
                    if (streamEnded) return false;
                    int read = stream.Read(buffer, 0, (int)Math.Min(buffer.Length, target - bufferStart));
                    if (read <= 0)
                    {
                        streamEnded = true;
                        return false;
                    }
                    bufferStart += read;
                }
                return true;
            }
        }

        private static byte[] Exact(BinaryReader reader, int length)
        {
            byte[] data = reader.ReadBytes(length);
            if (data.Length != length) throw new EndOfStreamException("回放文件被截断。");
            return data;
        }
    }
}
