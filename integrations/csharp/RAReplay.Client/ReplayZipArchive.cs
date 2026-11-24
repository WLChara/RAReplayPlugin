using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace RAReplay.Client
{
    // 压缩包读取的最小抽象：导入逻辑只依赖它，因此限额与防炸弹逻辑可以脱离具体的 zip 库测试。
    public interface IReplayZipEntry
    {
        // 条目名，保持压缩包内的原样（含目录部分）；无法可靠解码的字符以 U+FFFD 表示。
        string Name { get; }
        bool IsDirectory { get; }
        bool IsEncrypted { get; }
        // 头部声明的解压后大小，仅作参考（不可信）；未知为 -1。
        long DeclaredBytes { get; }
        // 解压数据流。读到末尾时必须校验 CRC，不一致抛异常。
        Stream Open();
    }

    public interface IReplayZipArchive : IDisposable
    {
        IList<IReplayZipEntry> Entries { get; }
    }

    // 基于 Ionic.Zip（DotNetZip 1.9.1.8，客户端 MapSharer 也在使用）的实现。只读取条目数据流，从不调用 Extract，
    // 因此条目名里的路径永远不会参与目标路径的计算。
    // 类库只在这个类里引用 Ionic.Zip：录制与播放路径不会加载它，导入压缩包时才由运行时按需解析。
    public sealed class IonicReplayZipArchive : IReplayZipArchive
    {
        private readonly Ionic.Zip.ZipFile zip;
        private readonly List<IReplayZipEntry> entries = new List<IReplayZipEntry>();

        private IonicReplayZipArchive(Ionic.Zip.ZipFile zip)
        {
            this.zip = zip;
            foreach (Ionic.Zip.ZipEntry entry in zip) entries.Add(new Entry(entry));
        }

        // 没有 UTF-8 标记的条目名按 GBK（936）解码，无效字节输出 U+FFFD；有标记的由 Ionic 按 UTF-8 解码。
        public static IReplayZipArchive Open(string path)
        {
            Ionic.Zip.ReadOptions options = new Ionic.Zip.ReadOptions();
            try
            {
                options.Encoding = Encoding.GetEncoding(936, EncoderFallback.ReplacementFallback,
                    new DecoderReplacementFallback(((char)0xFFFD).ToString()));
            }
            catch (Exception) { /* 没有 GBK 代码页时沿用 Ionic 的默认编码。 */ }
            Ionic.Zip.ZipFile zip = Ionic.Zip.ZipFile.Read(path, options);
            try { return new IonicReplayZipArchive(zip); }
            catch (Exception)
            {
                zip.Dispose();
                throw;
            }
        }

        public IList<IReplayZipEntry> Entries { get { return entries; } }

        public void Dispose() { zip.Dispose(); }

        private sealed class Entry : IReplayZipEntry
        {
            private readonly Ionic.Zip.ZipEntry entry;

            public Entry(Ionic.Zip.ZipEntry entry) { this.entry = entry; }

            public string Name { get { return entry.FileName; } }
            public bool IsDirectory { get { return entry.IsDirectory; } }
            public bool IsEncrypted { get { return entry.UsesEncryption; } }
            public long DeclaredBytes { get { return entry.UncompressedSize; } }

            public Stream Open()
            {
                return new CrcCheckedStream(entry.OpenReader(), entry.Crc, entry.UncompressedSize);
            }
        }

        // Ionic 的 OpenReader 只统计 CRC 不做核对；读到末尾时这里核对字节数与 CRC，损坏的条目因此不会被当作正常数据。
        private sealed class CrcCheckedStream : Stream
        {
            private readonly Ionic.Crc.CrcCalculatorStream inner;
            private readonly int expectedCrc;
            private readonly long expectedBytes;
            private bool verified;

            public CrcCheckedStream(Ionic.Crc.CrcCalculatorStream inner, int expectedCrc, long expectedBytes)
            {
                this.inner = inner;
                this.expectedCrc = expectedCrc;
                this.expectedBytes = expectedBytes;
            }

            public override int Read(byte[] buffer, int offset, int count)
            {
                int read = inner.Read(buffer, offset, count);
                if (read == 0 && count > 0 && !verified)
                {
                    verified = true;
                    if (inner.TotalBytesSlurped != expectedBytes || inner.Crc != expectedCrc)
                        throw new InvalidDataException("压缩包条目的大小或 CRC 校验不一致。");
                }
                return read;
            }

            protected override void Dispose(bool disposing)
            {
                if (disposing) inner.Dispose();
                base.Dispose(disposing);
            }

            public override bool CanRead { get { return true; } }
            public override bool CanSeek { get { return false; } }
            public override bool CanWrite { get { return false; } }
            public override long Length { get { throw new NotSupportedException(); } }
            public override long Position
            {
                get { throw new NotSupportedException(); }
                set { throw new NotSupportedException(); }
            }
            public override void Flush() { }
            public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
            public override void SetLength(long value) { throw new NotSupportedException(); }
            public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
        }
    }
}
