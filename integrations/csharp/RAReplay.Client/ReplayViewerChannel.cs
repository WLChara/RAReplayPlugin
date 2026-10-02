using System;
using System.IO;
using System.Text;

namespace RAReplay.Client
{
    /// <summary>单次播放独占的 RPVC 文件；仅传递当前请求的重播意图，不传递录像路径。</summary>
    public sealed class ReplayViewerChannel : IDisposable
    {
        public const uint HalfShroud = 1;
        private const uint Magic = 0x43565052;
        private const uint Version = 1;
        private readonly object gate = new object();
        private readonly byte[] requestId;
        private bool consumed;
        private bool disposed;

        private ReplayViewerChannel(string gamePath, byte[] requestId)
        {
            if (requestId == null || requestId.Length != 16)
                throw new ArgumentException("回放查看器请求 ID 必须为 16 字节。", nameof(requestId));
            this.requestId = (byte[])requestId.Clone();
            StringBuilder suffix = new StringBuilder(32);
            foreach (byte value in requestId) suffix.Append(value.ToString("x2"));
            ChannelPath = Path.Combine(Path.GetFullPath(gamePath), "rareplay-viewer-" + suffix + ".bin");
        }

        public string ChannelPath { get; private set; }

        /// <summary>启动游戏前原子发布 Ready。已有同名文件时拒绝覆盖。</summary>
        public static ReplayViewerChannel Create(string gamePath, byte[] requestId, uint viewerFlags = 0)
        {
            if ((viewerFlags & ~HalfShroud) != 0)
                throw new ArgumentOutOfRangeException(nameof(viewerFlags), "回放查看器选项包含未知位。");
            ReplayViewerChannel channel = new ReplayViewerChannel(gamePath, requestId);
            string temporary = channel.ChannelPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                using (FileStream output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                using (BinaryWriter writer = new BinaryWriter(output, Encoding.UTF8))
                {
                    writer.Write(Magic);
                    writer.Write(Version);
                    writer.Write(0u);
                    writer.Write(viewerFlags);
                    writer.Write(channel.requestId);
                    writer.Flush();
                    output.Flush(true);
                }
                File.Move(temporary, channel.ChannelPath);
                return channel;
            }
            finally
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
        }

        /// <summary>仅游戏真正退出后调用；匹配当前 ID 的合法 Restart 最多接受一次。</summary>
        public bool TryConsumeRestart(out uint viewerFlags)
        {
            lock (gate)
            {
                viewerFlags = 0;
                if (disposed || consumed) return false;
                uint action;
                uint flags;
                if (!TryReadOwned(out action, out flags) || action != 1) return false;
                consumed = true;
                viewerFlags = flags;
                return true;
            }
        }

        // 只删除本会话拥有的合法文件；内容被替换时保留现场并报告，阻止自动重开。
        public void Dispose()
        {
            lock (gate)
            {
                if (disposed) return;
                if (File.Exists(ChannelPath))
                {
                    uint action;
                    uint flags;
                    if (!TryReadOwned(out action, out flags))
                        throw new IOException("回放查看器控制文件已改变，不能删除非本会话文件。");
                    File.Delete(ChannelPath);
                }
                disposed = true;
            }
        }

        private bool TryReadOwned(out uint action, out uint flags)
        {
            action = 0;
            flags = 0;
            if (!File.Exists(ChannelPath)) return false;
            using (FileStream input = new FileStream(ChannelPath, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete))
            using (BinaryReader reader = new BinaryReader(input, Encoding.UTF8))
            {
                if (input.Length != 32 || reader.ReadUInt32() != Magic || reader.ReadUInt32() != Version)
                    return false;
                action = reader.ReadUInt32();
                flags = reader.ReadUInt32();
                if (action > 1 || (flags & ~HalfShroud) != 0) return false;
                byte[] id = reader.ReadBytes(16);
                for (int i = 0; i < requestId.Length; i++)
                    if (id[i] != requestId[i]) return false;
                return true;
            }
        }
    }
}
