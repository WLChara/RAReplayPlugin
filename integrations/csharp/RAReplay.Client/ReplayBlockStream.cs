using System;
using System.IO;
using System.IO.Compression;

namespace RAReplay.Client
{
    // RAREPL2 容器 v3 的块头（20 字节，小端）：magic、method、rawBytes、storedBytes、crc32。
    // 载荷为原样存放（method 0）或 RFC 1951 原始 deflate（method 1），crc32 是解压后内容的 CRC-32。
    internal struct ReplayBlockHeader
    {
        public uint Method;
        public uint RawBytes;
        public uint StoredBytes;
        public uint Crc32;

        // 块头加载荷占用的磁盘字节数。
        public long DiskBytes { get { return ReplayBlocks.HeaderBytes + (long)StoredBytes; } }
    }

    // 块解析、解码与 CRC-32。不打开文件，不持有状态。
    internal static class ReplayBlocks
    {
        public const int HeaderBytes = 20;
        public const uint Magic = 0x4B4C4252; // 字节为 R B L K
        public const uint MethodStored = 0;
        public const uint MethodDeflate = 1;
        public const uint ManifestMaxRawBytes = 16u * 1024 * 1024;
        public const uint FrameStreamMaxRawBytes = 4u * 1024 * 1024;
        // 逻辑帧流（各块解压结果拼接）的上限，与帧流段磁盘字节数的上限相同。
        public const long MaximumFrameStreamBytes = 1024L * 1024 * 1024;

        private static readonly uint[] CrcTable = CreateCrcTable();

        // 只看 20 字节块头，执行与载荷无关的全部规则：magic、method 只能是 0 或 1、rawBytes 为 1..maxRawBytes、
        // method 0 时 storedBytes 等于 rawBytes、method 1 时 1 ≤ storedBytes < rawBytes。
        public static bool TryParseHeader(byte[] data, int offset, uint maxRawBytes, out ReplayBlockHeader header)
        {
            header = new ReplayBlockHeader
            {
                Method = ReadUInt32(data, offset + 4),
                RawBytes = ReadUInt32(data, offset + 8),
                StoredBytes = ReadUInt32(data, offset + 12),
                Crc32 = ReadUInt32(data, offset + 16)
            };
            if (ReadUInt32(data, offset) != Magic) return false;
            if (header.RawBytes == 0 || header.RawBytes > maxRawBytes) return false;
            if (header.Method == MethodStored) return header.StoredBytes == header.RawBytes;
            if (header.Method == MethodDeflate) return header.StoredBytes >= 1 && header.StoredBytes < header.RawBytes;
            return false;
        }

        // 按已通过 TryParseHeader 的块头解码载荷 stored[storedOffset, storedOffset + storedBytes) 到 raw[0, rawBytes)，并核对 CRC-32。
        // raw 的容量至少为 rawBytes；返回 false 表示块无效（解码出错、长度不符或 CRC 不符）。
        public static bool TryDecodePayload(ReplayBlockHeader header, byte[] stored, int storedOffset, byte[] raw)
        {
            int rawBytes = (int)header.RawBytes;
            if (header.Method == MethodStored)
                Buffer.BlockCopy(stored, storedOffset, raw, 0, rawBytes);
            else if (!TryInflate(stored, storedOffset, (int)header.StoredBytes, raw, rawBytes))
                return false;
            return Crc32(raw, 0, rawBytes) == header.Crc32;
        }

        // 解码 data[offset, offset + available) 开头的一个块；available 之后的字节不读取。
        // 块头不足 20 字节、块头无效、载荷不完整、解码失败或 CRC 不符时返回 false。
        public static bool TryDecodeBlock(byte[] data, int offset, int available, uint maxRawBytes,
            out ReplayBlockHeader header, out byte[] raw)
        {
            raw = null;
            header = new ReplayBlockHeader();
            if (available < HeaderBytes || !TryParseHeader(data, offset, maxRawBytes, out header) ||
                header.DiskBytes > available)
                return false;
            byte[] decoded = new byte[header.RawBytes];
            if (!TryDecodePayload(header, data, offset + HeaderBytes, decoded)) return false;
            raw = decoded;
            return true;
        }

        // 清单段恰好是一个有效块，块头加载荷正好占满 metadataBytes（rawBytes 上限 16 MiB）；返回解压后的清单字节。
        // file 的当前位置为清单段段首；读取不足按文件被截断抛 EndOfStreamException。
        public static byte[] ReadManifest(Stream file, uint metadataBytes)
        {
            byte[] segment = new byte[metadataBytes];
            ReadExact(file, segment, 0, segment.Length);
            ReplayBlockHeader header;
            if (segment.Length < HeaderBytes || !TryParseHeader(segment, 0, ManifestMaxRawBytes, out header) ||
                header.DiskBytes != metadataBytes)
                throw new InvalidDataException("回放开局清单块的块头无效或与清单段长度不符。");
            byte[] raw = new byte[header.RawBytes];
            if (!TryDecodePayload(header, segment, HeaderBytes, raw))
                throw new InvalidDataException("回放开局清单块解码失败或 CRC 不符。");
            return raw;
        }

        // IEEE 802.3 CRC-32（反射多项式 0xEDB88320，初值与终值异或 0xFFFFFFFF），与 zlib crc32 相同。
        public static uint Crc32(byte[] data, int offset, int count)
        {
            uint crc = 0xFFFFFFFFu;
            int end = offset + count;
            for (int i = offset; i < end; i++) crc = CrcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFFu;
        }

        internal static void ReadExact(Stream file, byte[] buffer, int offset, int count)
        {
            while (count > 0)
            {
                int read = file.Read(buffer, offset, count);
                if (read <= 0) throw new EndOfStreamException("回放文件被截断。");
                offset += read;
                count -= read;
            }
        }

        // method 1 按 RFC 1951 原始 deflate 解码：一直解码到输入耗尽或 deflate 流结束；
        // 解码出错（InvalidDataException）、输出少于 rawBytes、或还能产生第 rawBytes+1 个字节，均为无效。
        // 不要求见到最终块标记，deflate 流结束之后载荷内的剩余字节不检查。
        // .NET Framework 的 DeflateStream 有两套解码器：目标框架 4.7.2 及以上的进程（DTAClient.exe 为 4.8）默认用 zlib，
        // 未声明目标框架或开启 Switch.System.IO.Compression.DoNotUseNativeZipLibraryForDecompression 时用框架自带的托管解码器。
        // 托管解码器在块中途输入耗尽时 Read 可能先返回 0、下一次才返回已解出的字节，因此连续两次返回 0 才视为结束
        // （探测第 rawBytes+1 个字节时同样适用）；zlib 解码器不会这样，同一判定对两者都成立。
        // 两侧解码器的已知残余差异来自托管解码器：对“距离超出已输出长度”“动态块码集不完整”
        // “固定块距离码 30/31”，zlib 报错而托管解码器不报错；只有人为构造、恰好解出 rawBytes 字节且 CRC-32 相符的此类载荷，两侧结论才可能不同。
        private static bool TryInflate(byte[] stored, int offset, int count, byte[] raw, int rawBytes)
        {
            try
            {
                using (MemoryStream input = new MemoryStream(stored, offset, count, false))
                using (DeflateStream inflate = new DeflateStream(input, CompressionMode.Decompress))
                {
                    int total = 0;
                    while (total < rawBytes)
                    {
                        int read = ReadUntilSecondZero(inflate, raw, total, rawBytes - total);
                        if (read == 0) return false;
                        total += read;
                    }
                    byte[] probe = new byte[1];
                    return ReadUntilSecondZero(inflate, probe, 0, 1) == 0;
                }
            }
            catch (InvalidDataException)
            {
                return false;
            }
        }

        // 第一次返回 0 时再读一次；两次都返回 0 才是解码结束。
        private static int ReadUntilSecondZero(Stream inflate, byte[] buffer, int offset, int count)
        {
            int read = inflate.Read(buffer, offset, count);
            return read != 0 ? read : inflate.Read(buffer, offset, count);
        }

        private static uint ReadUInt32(byte[] data, int offset)
        {
            return (uint)(data[offset] | data[offset + 1] << 8 | data[offset + 2] << 16 | data[offset + 3] << 24);
        }

        private static uint[] CreateCrcTable()
        {
            uint[] table = new uint[256];
            for (uint n = 0; n < 256; n++)
            {
                uint c = n;
                for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[n] = c;
            }
            return table;
        }
    }

    // 把 v3 帧流段（1 个或多个块首尾相接）呈现为只前进的只读逻辑流：逻辑偏移 0 是帧流头，按块顺序逐块解码，
    // 内存中只保留当前块。底层文件从 start 起顺序读取 diskBytes 字节，不回退。
    // 严格模式（完成文件、未完成录像已确认的有效块前缀）：任何块无效、块链没有正好结束在段尾都抛 InvalidDataException，
    // 不当作正常结束；只有最后一块正好结束在段尾时 Read 才返回 0。
    // 恢复模式（未完成录像的候选帧流段）：剩余不足 20 字节、块头无效、载荷不完整、解码失败或 CRC 不符时，
    // 在该块之前截止并按正常结束返回 0，其后全部忽略。
    // 两种模式下，只有通过块有效性判定的块才累计逻辑长度；累计超过 1 GiB 即抛 InvalidDataException（整份文件拒绝）。
    // 因块抛出的异常同时记入 Failure，调用方可据此区分块错误与帧级错误。
    // 底层读取不足（文件在读取期间变短）抛 EndOfStreamException，其他 I/O 异常原样抛出，均不当作正常结束。
    internal sealed class ReplayBlockStream : Stream
    {
        private readonly Stream file;
        private readonly bool recover;
        private readonly byte[] header = new byte[ReplayBlocks.HeaderBytes];
        private long diskRemaining;
        private byte[] stored = new byte[0];
        private byte[] raw = new byte[0];
        private int rawIndex;
        private int rawLength;
        private bool ended;

        public ReplayBlockStream(Stream file, long start, long diskBytes, bool recover)
        {
            if (file == null) throw new ArgumentNullException("file");
            if (diskBytes < 0) throw new ArgumentOutOfRangeException("diskBytes");
            this.file = file;
            this.recover = recover;
            diskRemaining = diskBytes;
            file.Position = start;
        }

        // 已解码的有效块前缀占用的磁盘字节数（块头加载荷）。
        public long ValidDiskBytes { get; private set; }

        // 已解码的有效块的逻辑字节数之和（含当前块尚未读出的部分）。
        public long LogicalBytes { get; private set; }

        public int BlockCount { get; private set; }

        // 恢复模式下截止的原因；正好读到段尾或尚未截止时为 null。
        public string StopReason { get; private set; }

        // 因块无效、块链与段长不符或逻辑帧流越限而抛出异常时的原因；之后的读取继续抛出同一错误。
        public string Failure { get; private set; }

        public override bool CanRead { get { return true; } }
        public override bool CanSeek { get { return false; } }
        public override bool CanWrite { get { return false; } }
        public override long Length { get { throw new NotSupportedException(); } }

        public override long Position
        {
            get { return LogicalBytes - (rawLength - rawIndex); }
            set { throw new NotSupportedException(); }
        }

        public override int Read(byte[] buffer, int offset, int count)
        {
            if (buffer == null) throw new ArgumentNullException("buffer");
            if (offset < 0 || count < 0 || count > buffer.Length - offset) throw new ArgumentOutOfRangeException("count");
            if (count == 0) return 0;
            while (rawIndex == rawLength)
                if (!NextBlock()) return 0;
            int copied = Math.Min(count, rawLength - rawIndex);
            Buffer.BlockCopy(raw, rawIndex, buffer, offset, copied);
            rawIndex += copied;
            return copied;
        }

        // 丢弃当前块剩余内容，继续解码其后的全部块直到段尾（严格模式）或首个无效块（恢复模式），
        // 用于确定有效块前缀并执行 1 GiB 上限。
        public void Drain()
        {
            rawIndex = rawLength;
            while (NextBlock()) rawIndex = rawLength;
        }

        private bool NextBlock()
        {
            if (Failure != null) throw new InvalidDataException(Failure);
            if (ended) return false;
            if (diskRemaining == 0)
            {
                ended = true;
                return false;
            }
            int index = BlockCount;
            if (diskRemaining < ReplayBlocks.HeaderBytes)
                return Stop("帧流段末尾剩余 " + diskRemaining + " 字节，不足一个块头（块 " + index + "）。");
            ReplayBlocks.ReadExact(file, header, 0, header.Length);
            ReplayBlockHeader block;
            if (!ReplayBlocks.TryParseHeader(header, 0, ReplayBlocks.FrameStreamMaxRawBytes, out block))
                return Stop("帧流块 " + index + " 的块头无效。");
            if (block.StoredBytes > diskRemaining - ReplayBlocks.HeaderBytes)
                return Stop("帧流块 " + index + " 的载荷不完整或超出帧流段。");
            if (stored.Length < block.StoredBytes) stored = new byte[block.StoredBytes];
            if (raw.Length < block.RawBytes) raw = new byte[block.RawBytes];
            ReplayBlocks.ReadExact(file, stored, 0, (int)block.StoredBytes);
            if (!ReplayBlocks.TryDecodePayload(block, stored, 0, raw))
                return Stop("帧流块 " + index + " 解码失败或 CRC 不符。");
            // 先判定本块有效，再累计；有效块累计超过 1 GiB 即拒绝整份文件。
            if (LogicalBytes + block.RawBytes > ReplayBlocks.MaximumFrameStreamBytes)
            {
                Failure = "逻辑帧流超过 1 GiB 上限。";
                throw new InvalidDataException(Failure);
            }
            diskRemaining -= block.DiskBytes;
            ValidDiskBytes += block.DiskBytes;
            LogicalBytes += block.RawBytes;
            BlockCount++;
            rawIndex = 0;
            rawLength = (int)block.RawBytes;
            return true;
        }

        private bool Stop(string reason)
        {
            rawIndex = 0;
            rawLength = 0;
            if (!recover)
            {
                Failure = reason;
                throw new InvalidDataException(reason);
            }
            ended = true;
            StopReason = reason;
            return false;
        }

        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
        public override void SetLength(long value) { throw new NotSupportedException(); }
        public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
    }
}
