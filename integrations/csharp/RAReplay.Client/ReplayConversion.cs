using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace RAReplay.Client
{
    // ===== 压缩旧回放 =====
    // 把回放目录中的 v2 完成文件无损转换为容器 v3。
    // 第一约束：原文件始终可恢复、回放内容逐字节不变。写出的 v3 文件只供本转换使用，格式与插件写出的容器 v3 相同。

    // 帧流切块策略，与插件 ArchiveBlockPolicy 相同：帧流头与每条帧记录各是一个不可拆分的单元；
    // 追加一个单元后，待写块达到 TargetRawBytes 字节或 MaxUnits 个单元即写出；追加会使待写块超过 4 MiB 时先写出当前块。
    internal sealed class ReplayBlockPolicy
    {
        public static readonly ReplayBlockPolicy Default = new ReplayBlockPolicy(64 * 1024, 600);

        public ReplayBlockPolicy(int targetRawBytes, int maxUnits)
        {
            if (targetRawBytes < 1 || targetRawBytes > ReplayBlocks.FrameStreamMaxRawBytes)
                throw new ArgumentOutOfRangeException("targetRawBytes");
            if (maxUnits < 1) throw new ArgumentOutOfRangeException("maxUnits");
            TargetRawBytes = targetRawBytes;
            MaxUnits = maxUnits;
        }

        public int TargetRawBytes { get; private set; }
        public int MaxUnits { get; private set; }
    }

    // 文件超出容器 v3 的上限（清单块超过 16 MiB、逻辑帧流或帧流段超过 1 GiB）：跳过该文件，不算失败。
    internal sealed class ReplayConversionLimitException : Exception
    {
        public ReplayConversionLimitException(string message) : base(message) { }
    }

    // 写出完成状态的容器 v3：先写 96 字节占位头，再写清单块与帧流块，Finish 时回到偏移 0 写正式头并 Flush(true)。
    // 块按写入端规则编码：DeflateStream（CompressionLevel.Optimal）的结果严格小于 rawBytes 时写 method 1，否则 method 0；
    // CRC-32 用 ReplayBlocks.Crc32。内容 SHA-256 随写出的磁盘字节（文件偏移 96 起）增量计算。
    internal sealed class ReplayContainerV3Writer : IDisposable
    {
        private const int HeaderBytes = 96;
        private readonly FileStream output;
        private readonly SHA256 sha = SHA256.Create();
        private readonly ReplayBlockPolicy policy;
        private readonly byte[] gameExeHash;
        private readonly uint metadataBytes;
        private byte[] pending = new byte[64 * 1024];
        private int pendingBytes;
        private int pendingUnits;
        private bool finished;

        // path 以 FileMode.CreateNew 创建（已存在即抛 IOException）；metadata 为原文件清单段的原始字节。
        public ReplayContainerV3Writer(string path, byte[] gameExeHash, byte[] metadata, ReplayBlockPolicy policy)
        {
            if (gameExeHash == null || gameExeHash.Length != 32) throw new ArgumentException("gamemd.exe SHA-256 必须为 32 字节。");
            if (metadata == null || metadata.Length == 0 || metadata.Length > ReplayBlocks.ManifestMaxRawBytes)
                throw new ReplayConversionLimitException("开局清单超过 16 MiB，无法转换为新格式。");
            this.policy = policy ?? ReplayBlockPolicy.Default;
            this.gameExeHash = (byte[])gameExeHash.Clone();
            byte[] payload;
            int payloadBytes;
            uint method = Encode(metadata, metadata.Length, out payload, out payloadBytes);
            long manifestBytes = ReplayBlocks.HeaderBytes + (long)payloadBytes;
            if (manifestBytes > ReplayLaunchManifest.MaximumBytes)
                throw new ReplayConversionLimitException("开局清单压缩后超过 16 MiB，无法转换为新格式。");
            metadataBytes = (uint)manifestBytes;
            output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None, 64 * 1024);
            try
            {
                output.Write(new byte[HeaderBytes], 0, HeaderBytes);
                WriteBlock(method, metadata, metadata.Length, payload, payloadBytes);
            }
            catch
            {
                output.Dispose();
                throw;
            }
        }

        // 逻辑帧流字节数（含待写块）。
        public long LogicalBytes { get; private set; }
        // 已写出的帧流段磁盘字节数。
        public long StoredBytes { get; private set; }
        public int BlockCount { get; private set; }

        // 追加一个不可拆分的单元（帧流头或一条帧记录）。
        public void AddUnit(byte[] buffer, int count)
        {
            if (finished) throw new InvalidOperationException("写入已结束。");
            if (count < 1) throw new ArgumentOutOfRangeException("count");
            if (count > ReplayBlocks.FrameStreamMaxRawBytes)
                throw new ReplayConversionLimitException("单条帧记录超过 4 MiB，无法转换为新格式。");
            if (LogicalBytes + count > ReplayBlocks.MaximumFrameStreamBytes)
                throw new ReplayConversionLimitException("帧流超过 1 GiB，无法转换为新格式。");
            if (pendingBytes + count > ReplayBlocks.FrameStreamMaxRawBytes) FlushBlock();
            if (pending.Length < pendingBytes + count)
                Array.Resize(ref pending, Math.Max(pendingBytes + count, Math.Min(pending.Length * 2, (int)ReplayBlocks.FrameStreamMaxRawBytes)));
            Buffer.BlockCopy(buffer, 0, pending, pendingBytes, count);
            pendingBytes += count;
            pendingUnits++;
            LogicalBytes += count;
            if (pendingBytes >= policy.TargetRawBytes || pendingUnits >= policy.MaxUnits) FlushBlock();
        }

        // 写出剩余部分，回到偏移 0 写正式头（flags=1，streamBytes 与内容 SHA-256 按实际写出的字节），并 Flush(true)。
        public void Finish()
        {
            if (finished) throw new InvalidOperationException("写入已结束。");
            FlushBlock();
            if (BlockCount == 0) throw new InvalidDataException("帧流为空，不能写出。");
            sha.TransformFinalBlock(new byte[0], 0, 0);
            using (MemoryStream header = new MemoryStream(HeaderBytes))
            using (BinaryWriter writer = new BinaryWriter(header))
            {
                writer.Write(Encoding.ASCII.GetBytes("RAREPL2\0"));
                writer.Write(3u);
                writer.Write((uint)HeaderBytes);
                writer.Write(metadataBytes);
                writer.Write(1u);
                writer.Write((ulong)StoredBytes);
                writer.Write(gameExeHash);
                writer.Write(sha.Hash);
                writer.Flush();
                output.Position = 0;
                output.Write(header.ToArray(), 0, HeaderBytes);
            }
            output.Flush(true);
            finished = true;
        }

        private void FlushBlock()
        {
            if (pendingBytes == 0) return;
            byte[] payload;
            int payloadBytes;
            uint method = Encode(pending, pendingBytes, out payload, out payloadBytes);
            if (StoredBytes + ReplayBlocks.HeaderBytes + payloadBytes > ReplayBlocks.MaximumFrameStreamBytes)
                throw new ReplayConversionLimitException("压缩后的帧流段超过 1 GiB，无法转换为新格式。");
            WriteBlock(method, pending, pendingBytes, payload, payloadBytes);
            StoredBytes += ReplayBlocks.HeaderBytes + payloadBytes;
            BlockCount++;
            pendingBytes = 0;
            pendingUnits = 0;
        }

        // 压缩 raw[0, rawBytes)：结果严格小于 rawBytes 时返回 method 1 与压缩数据，否则返回 method 0 与 raw 本身。
        private static uint Encode(byte[] raw, int rawBytes, out byte[] payload, out int payloadBytes)
        {
            using (MemoryStream packed = new MemoryStream())
            {
                using (DeflateStream deflate = new DeflateStream(packed, CompressionLevel.Optimal, true))
                    deflate.Write(raw, 0, rawBytes);
                if (packed.Length < rawBytes)
                {
                    payload = packed.GetBuffer();
                    payloadBytes = (int)packed.Length;
                    return ReplayBlocks.MethodDeflate;
                }
            }
            payload = raw;
            payloadBytes = rawBytes;
            return ReplayBlocks.MethodStored;
        }

        // 块头与载荷连续写出，并计入内容 SHA-256。
        private void WriteBlock(uint method, byte[] raw, int rawBytes, byte[] payload, int payloadBytes)
        {
            byte[] header = new byte[ReplayBlocks.HeaderBytes];
            Put(header, 0, ReplayBlocks.Magic);
            Put(header, 4, method);
            Put(header, 8, (uint)rawBytes);
            Put(header, 12, (uint)payloadBytes);
            Put(header, 16, ReplayBlocks.Crc32(raw, 0, rawBytes));
            output.Write(header, 0, header.Length);
            output.Write(payload, 0, payloadBytes);
            sha.TransformBlock(header, 0, header.Length, null, 0);
            sha.TransformBlock(payload, 0, payloadBytes, null, 0);
        }

        private static void Put(byte[] data, int offset, uint value)
        {
            data[offset] = (byte)value;
            data[offset + 1] = (byte)(value >> 8);
            data[offset + 2] = (byte)(value >> 16);
            data[offset + 3] = (byte)(value >> 24);
        }

        public void Dispose()
        {
            output.Dispose();
            sha.Dispose();
        }
    }

    public enum ReplayConversionOutcome { Converted, Skipped, Failed }

    // 单个文件的结果。Reason 是给玩家看的简体中文；Detail 是写进客户端日志的技术细节。宿主只读取，不创建、不修改。
    public sealed class ReplayConversionItem
    {
        public string Path { get; internal set; }
        public ReplayConversionOutcome Outcome { get; internal set; }
        public string Reason { get; internal set; }
        public string Detail { get; internal set; }
        public long OriginalBytes { get; internal set; }
        public long ConvertedBytes { get; internal set; }
        // 原文件未能移到回收站时保留的 .v2bak 完整路径；否则为 null。
        public string KeptBackupPath { get; internal set; }

        public string FileName { get { return System.IO.Path.GetFileName(Path ?? string.Empty); } }
    }

    public sealed class ReplayConversionResult
    {
        private readonly List<ReplayConversionItem> items = new List<ReplayConversionItem>();

        // 只由转换器（与测试）创建；宿主只读取结果。
        internal ReplayConversionResult(int total) { Total = total; }

        public int Total { get; private set; }
        public IList<ReplayConversionItem> Items { get { return items; } }
        // 玩家停止后未处理的数量；没有停止时为 0。
        public int NotProcessed { get; private set; }
        public bool Stopped { get; private set; }
        // 本次开始前清理掉的残留临时文件数。
        public int CleanedTemporaryFiles { get; internal set; }
        // 开始前发现、但对应的 .yrrep 不存在而保留的 .yrrep.v3tmp（完整路径）：可能是上次替换双重失败后留下的唯一完整副本。
        public IList<string> KeptTemporaryFiles { get { return keptTemporaryFiles; } }
        private readonly List<string> keptTemporaryFiles = new List<string>();

        public IEnumerable<ReplayConversionItem> Converted { get { return items.Where(item => item.Outcome == ReplayConversionOutcome.Converted); } }

        internal void Add(ReplayConversionItem item) { items.Add(item); }

        internal void MarkStopped(int notProcessed)
        {
            Stopped = true;
            NotProcessed = notProcessed;
        }

        // 汇总文字：已压缩的数量与“原大小 → 新大小”；回收站可释放的空间；未能移到回收站而保留的 .v2bak；
        // 用户停止时剩余未处理的数量；跳过与失败的数量及前 maxListed 条原因；开始前保留的无法确认的临时文件。
        public string BuildSummary(int maxListed)
        {
            string newLine = Environment.NewLine;
            StringBuilder text = new StringBuilder();
            List<ReplayConversionItem> converted = Converted.ToList();
            if (converted.Count == 0) text.Append("没有压缩任何回放。");
            else
            {
                text.Append("已压缩 ").Append(Count(converted.Count)).Append(" 个回放：")
                    .Append(ReplayDetails.FormatBytes(converted.Sum(item => item.OriginalBytes))).Append(" → ")
                    .Append(ReplayDetails.FormatBytes(converted.Sum(item => item.ConvertedBytes))).Append("。");
                List<ReplayConversionItem> recycled = converted.Where(item => item.KeptBackupPath == null).ToList();
                if (recycled.Count > 0)
                    text.Append(newLine).Append("原文件已移到回收站，清空回收站后可释放约 ")
                        .Append(ReplayDetails.FormatBytes(recycled.Sum(item => item.OriginalBytes))).Append("。");
                List<ReplayConversionItem> kept = converted.Where(item => item.KeptBackupPath != null).ToList();
                if (kept.Count > 0)
                {
                    text.Append(newLine).Append(newLine).Append("以下 ").Append(Count(kept.Count))
                        .Append(" 个原文件未能移到回收站，保留在回放文件夹中（回放本身已压缩，可自行删除这些文件）：");
                    AppendList(text, kept.Select(item => "原文件保留为 " + System.IO.Path.GetFileName(item.KeptBackupPath) + "（未能移到回收站）"), maxListed);
                }
            }
            if (Stopped)
                text.Append(newLine).Append(newLine).Append("已按要求停止，还有 ").Append(Count(NotProcessed)).Append(" 个回放未处理。");
            List<ReplayConversionItem> skipped = items.Where(item => item.Outcome == ReplayConversionOutcome.Skipped).ToList();
            List<ReplayConversionItem> failed = items.Where(item => item.Outcome == ReplayConversionOutcome.Failed).ToList();
            if (skipped.Count + failed.Count > 0)
            {
                text.Append(newLine).Append(newLine).Append("跳过 ").Append(Count(skipped.Count)).Append(" 个，失败 ")
                    .Append(Count(failed.Count)).Append(" 个（这些回放保持原样）：");
                AppendList(text, failed.Concat(skipped).Select(item => ReplayImportResult.Shorten(item.FileName, 40) + "：" + item.Reason), maxListed);
            }
            if (keptTemporaryFiles.Count > 0)
            {
                List<string> names = keptTemporaryFiles.Select(path => System.IO.Path.GetFileName(path)).ToList();
                int shown = Math.Min(names.Count, Math.Max(1, maxListed));
                string listed = string.Join("、", names.Take(shown)) +
                    (names.Count > shown ? " 等 " + Count(names.Count - shown) + " 个" : string.Empty);
                text.Append(newLine).Append(newLine).Append("发现 ").Append(Count(names.Count)).Append(" 个无法确认的临时文件（")
                    .Append(listed).Append("），可能是上次压缩失败后留下的唯一副本，已保留，请自行检查。");
            }
            return text.ToString();
        }

        private static void AppendList(StringBuilder text, IEnumerable<string> lines, int maxListed)
        {
            List<string> all = lines.ToList();
            int shown = Math.Min(all.Count, Math.Max(0, maxListed));
            for (int i = 0; i < shown; i++) text.Append(Environment.NewLine).Append("- ").Append(all[i]);
            if (all.Count > shown)
                text.Append(Environment.NewLine).Append("等 ").Append(Count(all.Count - shown)).Append(" 个（未全部列出）");
        }

        private static string Count(int value)
        {
            return value.ToString(CultureInfo.InvariantCulture);
        }
    }

    public sealed class ReplayConversionOptions
    {
        public string ReplaysDirectory { get; set; }
        // 把替换下来的原文件（.v2bak 完整路径）移到回收站；返回 null 表示成功，否则为失败原因。
        // 必须提供：正式代码传 ReplayFileOperations.MoveToRecycleBin，测试传自己的实现，测试不得触碰真实回收站。
        public Func<string, string> MoveToRecycleBin { get; set; }
        // 为空时用生产切块策略（64 KiB / 600 单元）。块策略不对宿主公开，只有测试会注入。
        internal ReplayBlockPolicy BlockPolicy { get; set; }
        // 每个文件开始前查询；返回 true 时停止（不中断正在处理的文件）。
        public Func<bool> StopRequested { get; set; }
        // 每个文件开始前报告（从 1 起的序号，总数）；在工作线程上调用。
        public Action<int, int> Progress { get; set; }
        // 测试钩子：临时文件写完之后、第 4 步核对之前调用，参数为临时文件完整路径（模拟转换代码写错）。正式代码不设置。
        internal Action<string> AfterWriteTemporary { get; set; }
        // 测试钩子：临时文件核对通过之后、替换前再核对原文件之前调用，参数为原文件完整路径。正式代码不设置。
        internal Action<string> BeforeReplace { get; set; }
    }

    public static class ReplayConverter
    {
        internal const string TemporarySuffix = ".v3tmp";
        internal const string BackupSuffix = ".v2bak";
        internal const string ChangedText = "文件在压缩期间被改动";
        private const int CompareBufferBytes = 64 * 1024;

        // 候选：容器 v2、完成文件（flags=1）、后缀为 .yrrep。未完成录像与已是 v3 的文件都不转换。
        public static bool IsCandidate(ReplayArchive replay)
        {
            return replay != null && replay.ContainerVersion == 2 && !replay.IsUnfinalized &&
                replay.Path != null && replay.Path.EndsWith(".yrrep", StringComparison.OrdinalIgnoreCase);
        }

        // 逐个顺序转换（应在后台线程调用）；开始前清理残留的 .yrrep.v3tmp。永不因单个文件抛出异常。
        public static ReplayConversionResult Convert(IList<ReplayArchive> candidates, ReplayConversionOptions options)
        {
            if (options == null || string.IsNullOrEmpty(options.ReplaysDirectory)) throw new ArgumentException("缺少回放目录。");
            if (options.MoveToRecycleBin == null) throw new ArgumentException("必须提供回收站操作。");
            ReplayConversionResult result = new ReplayConversionResult(candidates.Count);
            result.CleanedTemporaryFiles = CleanTemporaryFiles(options.ReplaysDirectory, result.KeptTemporaryFiles);
            for (int i = 0; i < candidates.Count; i++)
            {
                if (options.StopRequested != null && options.StopRequested())
                {
                    result.MarkStopped(candidates.Count - i);
                    break;
                }
                if (options.Progress != null) options.Progress(i + 1, candidates.Count);
                ReplayConversionItem item;
                try { item = ConvertOne(candidates[i], options); }
                catch (Exception ex)
                {
                    item = new ReplayConversionItem
                    {
                        Path = candidates[i] == null ? null : candidates[i].Path,
                        Outcome = ReplayConversionOutcome.Failed,
                        Reason = "压缩时出现意外错误",
                        Detail = ex.ToString()
                    };
                }
                result.Add(item);
            }
            return result;
        }

        // 清理回放目录中直接位于其下、后缀为 .yrrep.v3tmp 的残留临时文件（只由转换器产生）。
        // 只删除对应的 .yrrep（去掉 .v3tmp 后缀的路径）仍然存在的：这时原文件完好，临时文件是未经核对的半成品。
        // 对应的 .yrrep 不存在时保留并记入 kept（完整路径）：第 6 步替换双重失败后，它可能是唯一完整的副本。
        // .v2bak 是用户原文件，从不自动处理。删除失败的文件留待下次。返回删除的数量。
        internal static int CleanTemporaryFiles(string directory, ICollection<string> kept)
        {
            int removed = 0;
            try
            {
                if (!Directory.Exists(directory)) return 0;
                foreach (string file in Directory.EnumerateFiles(directory, "*" + TemporarySuffix, SearchOption.TopDirectoryOnly))
                {
                    if (!file.EndsWith(".yrrep" + TemporarySuffix, StringComparison.OrdinalIgnoreCase)) continue;
                    string original = file.Substring(0, file.Length - TemporarySuffix.Length);
                    if (!File.Exists(original))
                    {
                        if (kept != null) kept.Add(file);
                        continue;
                    }
                    try
                    {
                        File.Delete(file);
                        removed++;
                    }
                    catch (Exception) { /* 正被占用或无权删除时留待下次。 */ }
                }
            }
            catch (Exception) { /* 目录无法枚举时不影响转换。 */ }
            return removed;
        }

        // 单个文件，分 8 个步骤（见方法内的编号注释）。replay 为列表中显示的回放；返回结果，不抛出可预期的文件错误。
        internal static ReplayConversionItem ConvertOne(ReplayArchive replay, ReplayConversionOptions options)
        {
            ReplayConversionItem item = new ReplayConversionItem { Path = replay.Path };
            if (!IsCandidate(replay))
                return Skip(item, replay.IsUnfinalized ? "未完成录像不压缩" :
                    replay.ContainerVersion != 2 ? "已是新格式" : "只压缩后缀为 .yrrep 的回放", null);

            // 1. 检查：路径规则、不是重分析点、与列表中显示的一致、能独占打开；记录字节数与时间戳。
            string fullPath, pathError;
            if (!ReplayFileOperations.TryGetDeletablePath(options.ReplaysDirectory, replay.Path, out fullPath, out pathError))
                return Skip(item, "只压缩直接位于回放文件夹中的回放", pathError);
            item.Path = fullPath;
            string temporary = fullPath + TemporarySuffix;
            string backup = fullPath + BackupSuffix;
            long originalBytes;
            DateTime created, written;
            try
            {
                if (!File.Exists(fullPath)) return Skip(item, "回放文件已不存在", null);
                if ((File.GetAttributes(fullPath) & FileAttributes.ReparsePoint) != 0)
                    return Skip(item, "该文件是符号链接或重分析点，不压缩", null);
                // File.Replace 会覆盖已存在的备份名：同名 .v2bak 可能是用户以前的原文件，不能覆盖。
                if (File.Exists(backup) || Directory.Exists(backup))
                    return Skip(item, "回放文件夹里已有同名的 " + System.IO.Path.GetFileName(backup) + "，为免覆盖它，未压缩", null);
                if (!replay.MatchesCurrentHeader()) return Skip(item, "回放文件已被改动，与列表中显示的不一致，请刷新列表", null);
                using (new FileStream(fullPath, FileMode.Open, FileAccess.Read, FileShare.None)) { }
                FileInfo info = new FileInfo(fullPath);
                originalBytes = info.Length;
                created = info.CreationTimeUtc;
                written = info.LastWriteTimeUtc;
            }
            catch (UnauthorizedAccessException ex) { return Skip(item, "没有权限读取该回放文件", ex.ToString()); }
            catch (FileNotFoundException ex) { return Skip(item, "回放文件已不存在", ex.ToString()); }
            catch (EndOfStreamException ex) { return Skip(item, "回放文件无法读取，未压缩", ex.ToString()); }
            catch (IOException ex) { return Skip(item, "回放文件正被占用（可能正在播放），未压缩", ex.ToString()); }
            catch (InvalidDataException ex) { return Skip(item, "回放文件无法读取，未压缩", ex.ToString()); }
            item.OriginalBytes = originalBytes;

            // 2–3. 重新打开（要求容器 v2、完成文件），严格校验的同时按帧记录边界切块写临时文件。
            ReplayArchive archive;
            try { archive = ReplayArchive.Open(fullPath); }
            catch (Exception ex) { return Skip(item, "回放文件无法读取，未压缩", ex.ToString()); }
            if (archive.ContainerVersion != 2 || archive.IsUnfinalized)
                return Skip(item, archive.IsUnfinalized ? "未完成录像不压缩" : "已是新格式", null);
            bool temporaryCreated = false;
            try
            {
                string writeDetail;
                ReplayConversionItem failure = WriteTemporary(archive, temporary, options.BlockPolicy, item, out temporaryCreated, out writeDetail);
                if (failure != null) return failure;
                if (options.AfterWriteTemporary != null) options.AfterWriteTemporary(temporary);

                // 4. 核对临时文件：任何一项不符说明转换代码有缺陷。
                string mismatch;
                try { mismatch = VerifyConverted(archive, temporary); }
                catch (Exception ex) { mismatch = "核对时出错：" + ex; }
                if (mismatch != null)
                    return Fail(item, "压缩结果核对不一致，已放弃，原文件未改动",
                        "转换器写出的 v3 文件与原文件不一致（转换代码缺陷）：" + mismatch);

                if (options.BeforeReplace != null) options.BeforeReplace(fullPath);

                // 5. 替换前再核对原文件：字节数、最后写入时间与头部都未变化。
                FileInfo now = new FileInfo(fullPath);
                bool unchanged;
                try { unchanged = now.Exists && now.Length == originalBytes && now.LastWriteTimeUtc == written && archive.MatchesCurrentHeader(); }
                catch (Exception) { unchanged = false; }
                if (!unchanged) return Skip(item, ChangedText, "替换前核对：字节数、最后写入时间或容器头与压缩开始时不同。");
                if (File.Exists(backup) || Directory.Exists(backup))
                    return Skip(item, "回放文件夹里已有同名的 " + System.IO.Path.GetFileName(backup) + "，为免覆盖它，未压缩", null);

                // 6. 原文件名始终指向完整文件（旧或新）：替换后原文件成为 .v2bak。
                try { File.Replace(temporary, fullPath, backup); }
                catch (Exception ex)
                {
                    ReplayConversionItem failed = ReplaceFailed(item, fullPath, backup, temporary, ex);
                    // 原文件名不再指向完整文件时保留已核对的临时文件，不删除任何一份。
                    temporaryCreated = File.Exists(fullPath);
                    return failed;
                }
                temporaryCreated = false;
                item.Outcome = ReplayConversionOutcome.Converted;
                item.Reason = "已压缩";
                item.ConvertedBytes = new FileInfo(fullPath).Length;
                StringBuilder detail = new StringBuilder(writeDetail);
                try
                {
                    File.SetCreationTimeUtc(fullPath, created);
                    File.SetLastWriteTimeUtc(fullPath, written);
                }
                catch (Exception ex) { detail.Append("；未能恢复文件时间：").Append(ex.Message); }

                // 7–8. 原文件移到回收站（可注入）；失败时保留 .v2bak。
                string recycleError;
                try { recycleError = options.MoveToRecycleBin(backup); }
                catch (Exception ex) { recycleError = ex.Message; }
                if (recycleError != null || File.Exists(backup))
                {
                    item.KeptBackupPath = backup;
                    detail.Append("；原文件未能移到回收站，保留为 ").Append(backup).Append("：").Append(recycleError ?? "文件仍然存在");
                }
                item.Detail = detail.ToString();
                return item;
            }
            finally
            {
                if (temporaryCreated) TryDelete(temporary);
            }
        }

        // 第 2–3 步：ValidatePayload 的严格逐帧扫描报告每个单元的结束位置，写入端从原文件的逻辑帧流按这些边界读出单元。
        // 校验失败或超出上限时返回跳过结果；成功时返回 null，writeDetail 为写出统计。
        private static ReplayConversionItem WriteTemporary(ReplayArchive archive, string temporary, ReplayBlockPolicy policy,
            ReplayConversionItem item, out bool temporaryCreated, out string writeDetail)
        {
            temporaryCreated = false;
            writeDetail = null;
            byte[] metadata;
            try { metadata = archive.ReadMetadata(); }
            catch (Exception ex) { return Skip(item, "回放文件无法读取，未压缩", ex.ToString()); }
            ReplayContainerV3Writer writer;
            try { writer = new ReplayContainerV3Writer(temporary, archive.GameExeHash, metadata, policy); }
            catch (ReplayConversionLimitException ex) { return Skip(item, ex.Message, null); }
            catch (Exception ex) { return Fail(item, "无法在回放文件夹中创建临时文件", ex.ToString()); }
            temporaryCreated = true;
            try
            {
                using (writer)
                using (FileStream source = new FileStream(archive.Path, FileMode.Open, FileAccess.Read, FileShare.Read,
                    64 * 1024, FileOptions.SequentialScan))
                {
                    Stream logical = archive.OpenLogicalStream(source);
                    byte[] unit = new byte[64 * 1024];
                    long previous = 0;
                    archive.ValidatePayload(end =>
                    {
                        long size = end - previous;
                        if (size <= 0 || size > ReplayBlocks.FrameStreamMaxRawBytes)
                            throw new ReplayConversionLimitException("单条帧记录超过 4 MiB，无法转换为新格式。");
                        if (unit.Length < size) unit = new byte[size];
                        ReplayBlocks.ReadExact(logical, unit, 0, (int)size);
                        writer.AddUnit(unit, (int)size);
                        previous = end;
                    });
                    if (previous != archive.StreamBytes)
                        throw new InvalidDataException("逐帧校验结束于 " + previous + "，帧流共 " + archive.StreamBytes + " 字节。");
                    writer.Finish();
                    writeDetail = "原 " + item.OriginalBytes + " 字节，逻辑帧流 " + writer.LogicalBytes + " 字节，帧流段 " +
                        writer.StoredBytes + " 字节，" + writer.BlockCount + " 块";
                }
                return null;
            }
            catch (ReplayConversionLimitException ex) { return Skip(item, ex.Message, null); }
            catch (InvalidDataException ex) { return Skip(item, "回放校验未通过，未压缩", ex.ToString()); }
            catch (EndOfStreamException ex) { return Skip(item, "回放校验未通过，未压缩", ex.ToString()); }
            catch (Exception ex) { return Fail(item, "写入临时文件时出错，可能磁盘空间不足", ex.ToString()); }
        }

        // 第 4 步：临时文件 Open 加 ValidatePayload 通过；容器 v3、完成文件；gamemd.exe SHA-256 相同；
        // 解压后的清单与原文件 metadata 逐字节相同；逻辑帧流与原文件帧流逐字节相同（两边流式比较）。返回不符的说明或 null。
        private static string VerifyConverted(ReplayArchive original, string temporary)
        {
            ReplayArchive converted = ReplayArchive.Open(temporary);
            converted.ValidatePayload();
            if (converted.ContainerVersion != 3 || converted.IsUnfinalized)
                return "容器版本 " + converted.ContainerVersion + "，未完成=" + converted.IsUnfinalized;
            if (!ReplayLaunchManifest.EqualsHash(converted.GameExeHash, original.GameExeHash)) return "gamemd.exe SHA-256 不同";
            if (!converted.ReadMetadata().SequenceEqual(original.ReadMetadata())) return "解压后的开局清单与原文件不同";
            using (FileStream left = new FileStream(original.Path, FileMode.Open, FileAccess.Read, FileShare.Read,
                CompareBufferBytes, FileOptions.SequentialScan))
            using (FileStream right = new FileStream(temporary, FileMode.Open, FileAccess.Read, FileShare.Read,
                CompareBufferBytes, FileOptions.SequentialScan))
                return CompareStreams(original.OpenLogicalStream(left), converted.OpenLogicalStream(right));
        }

        // 逐块读满两边的缓冲后比较；返回第一处不同的说明，相同返回 null。
        internal static string CompareStreams(Stream expected, Stream actual)
        {
            byte[] left = new byte[CompareBufferBytes];
            byte[] right = new byte[CompareBufferBytes];
            long position = 0;
            while (true)
            {
                int leftBytes = ReadFull(expected, left);
                int rightBytes = ReadFull(actual, right);
                int common = Math.Min(leftBytes, rightBytes);
                for (int i = 0; i < common; i++)
                    if (left[i] != right[i]) return "逻辑帧流在偏移 " + (position + i) + " 处不同";
                if (leftBytes != rightBytes)
                    return "逻辑帧流长度不同：原文件 " + (position + leftBytes) + (leftBytes < left.Length ? "" : "+") +
                        " 字节，新文件 " + (position + rightBytes) + (rightBytes < right.Length ? "" : "+") + " 字节";
                if (leftBytes < left.Length) return null;
                position += leftBytes;
            }
        }

        private static int ReadFull(Stream stream, byte[] buffer)
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

        // 第 6 步失败：File.Replace 通常保持原文件不变（只读或被占用时实测如此）；少数错误（ERROR_UNABLE_TO_MOVE_REPLACEMENT_2）
        // 会让原文件已改名为备份名，这时先把备份改回原名，仍不行再把已核对的新文件放到原名，保证原文件名指向完整文件。
        // 临时文件由调用方在原文件名指向完整文件时删除。
        private static ReplayConversionItem ReplaceFailed(ReplayConversionItem item, string fullPath, string backup, string temporary,
            Exception error)
        {
            StringBuilder detail = new StringBuilder("File.Replace 失败：").Append(error);
            string where = null;
            if (!File.Exists(fullPath))
            {
                try
                {
                    if (File.Exists(backup)) File.Move(backup, fullPath);
                    else if (File.Exists(temporary)) File.Move(temporary, fullPath);
                }
                catch (Exception moveError) { detail.Append("；恢复原文件名失败：").Append(moveError); }
                if (!File.Exists(fullPath))
                    where = File.Exists(backup) ? backup : File.Exists(temporary) ? temporary : null;
            }
            item.Detail = detail.ToString();
            item.Outcome = ReplayConversionOutcome.Failed;
            item.Reason = where == null ? "替换原文件失败（文件可能是只读或正被占用），原文件未改动" :
                "替换原文件失败，原文件现在名为 " + System.IO.Path.GetFileName(where);
            return item;
        }

        private static ReplayConversionItem Skip(ReplayConversionItem item, string reason, string detail)
        {
            item.Outcome = ReplayConversionOutcome.Skipped;
            item.Reason = reason;
            item.Detail = detail;
            return item;
        }

        private static ReplayConversionItem Fail(ReplayConversionItem item, string reason, string detail)
        {
            item.Outcome = ReplayConversionOutcome.Failed;
            item.Reason = reason;
            item.Detail = detail;
            return item;
        }

        private static void TryDelete(string path)
        {
            try { if (File.Exists(path)) File.Delete(path); }
            catch (Exception) { /* 留待下次开始时清理。 */ }
        }
    }
}
