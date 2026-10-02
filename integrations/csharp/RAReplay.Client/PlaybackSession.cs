using System;
using System.IO;

namespace RAReplay.Client
{
    public sealed class PlaybackSession : IDisposable
    {
        private readonly string root;
        private readonly object disposeGate = new object();
        private readonly string spawnIniPath;
        private readonly string spawnMapPath;
        private readonly byte[] originalIni;
        private readonly byte[] originalMap;
        private byte[] appliedIniHash;
        private byte[] appliedMapHash;
        private byte[] request;
        private ReplayViewerChannel viewerChannel;
        private bool disposed;

        private PlaybackSession(string root, ReplayArchive archive)
        {
            this.root = root;
            spawnIniPath = Path.Combine(root, "spawn.ini");
            spawnMapPath = Path.Combine(root, "spawnmap.ini");
            originalIni = File.Exists(spawnIniPath) ? File.ReadAllBytes(spawnIniPath) : null;
            originalMap = File.Exists(spawnMapPath) ? File.ReadAllBytes(spawnMapPath) : null;
            ReplayPath = archive.Path;
            Manifest = archive.Manifest;
        }

        public string ReplayPath { get; private set; }
        public ReplayLaunchManifest Manifest { get; private set; }

        internal static PlaybackSession Prepare(string root, ReplayArchive archive, uint viewerFlags)
        {
            PlaybackSession session = new PlaybackSession(root, archive);
            try
            {
                byte[] replayIni = MakeLocalEndpoints(archive.Manifest.SpawnIniBytes);
                byte[] replayMap = archive.Manifest.SpawnMapBytes;
                session.appliedIniHash = ReplayLaunchManifest.Hash(replayIni);
                session.appliedMapHash = ReplayLaunchManifest.Hash(replayMap);
                AtomicWrite(session.spawnIniPath, replayIni);
                AtomicWrite(session.spawnMapPath, replayMap);
                ReplayRuntimeRequest runtimeRequest = ReplayRuntimeRequest.Create(
                    ReplayRuntimeRequest.Play, archive.Path, new byte[0]);
                session.request = runtimeRequest.Serialize();
                ReplayRuntimeRequest.Publish(root, session.request);
                session.viewerChannel = ReplayViewerChannel.Create(root, runtimeRequest.RequestId, viewerFlags);
                return session;
            }
            catch (Exception prepareError)
            {
                try { session.Dispose(); }
                catch (Exception restoreError)
                {
                    throw new AggregateException("准备回放失败，且启动文件恢复失败。",
                        prepareError, restoreError);
                }
                throw;
            }
        }

        // 退出处理先消费意图，再成功 Dispose，宿主才能在 UI 线程启动新会话。
        public bool TryConsumeRestart(out uint viewerFlags)
        {
            lock (disposeGate)
            {
                viewerFlags = 0;
                return !disposed && viewerChannel != null && viewerChannel.TryConsumeRestart(out viewerFlags);
            }
        }

        // 宿主应在游戏真正退出或启动失败后调用；恢复只触碰本会话写入的文件。
        public void Dispose()
        {
            lock (disposeGate)
            {
                if (disposed) return;
                Exception failure = null;
                try { RestoreOne(spawnIniPath, originalIni, appliedIniHash); }
                catch (Exception error) { failure = error; }
                try { RestoreOne(spawnMapPath, originalMap, appliedMapHash); }
                catch (Exception error) { if (failure == null) failure = error; }
                try
                {
                    if (request != null) ReplayRuntimeRequest.DeleteIfOwned(root, request);
                }
                catch (Exception error) { if (failure == null) failure = error; }
                try { if (viewerChannel != null) viewerChannel.Dispose(); }
                catch (Exception error) { if (failure == null) failure = error; }
                if (failure != null) throw new IOException("回放退出后恢复启动文件失败。", failure);
                disposed = true;
            }
        }

        private static void RestoreOne(string path, byte[] original, byte[] appliedHash)
        {
            if (appliedHash == null) return;
            if (File.Exists(path) &&
                !ReplayLaunchManifest.EqualsHash(ReplayLaunchManifest.HashFile(path), appliedHash)) return;
            if (original == null)
            {
                if (File.Exists(path)) File.Delete(path);
            }
            else AtomicWrite(path, original);
        }

        private static void AtomicWrite(string path, byte[] bytes)
        {
            string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                using (FileStream stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    stream.Write(bytes, 0, bytes.Length);
                    stream.Flush(true);
                }
                if (File.Exists(path)) File.Replace(temporary, path, null);
                else File.Move(temporary, path);
            }
            finally
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
        }

        internal static byte[] MakeLocalEndpoints(byte[] spawnIni)
        {
            // ISO-8859-1 一字节映射保持非连接字段原始字节；仅替换现有网络端点。
            System.Text.Encoding bytes = System.Text.Encoding.GetEncoding(28591);
            string content = bytes.GetString(spawnIni);
            string section = string.Empty;
            string[] lines = System.Text.RegularExpressions.Regex.Split(content, "(?<=\\n)");
            for (int i = 0; i < lines.Length; i++)
            {
                string ending = lines[i].EndsWith("\r\n", StringComparison.Ordinal) ? "\r\n" :
                    lines[i].EndsWith("\n", StringComparison.Ordinal) ? "\n" : string.Empty;
                string line = lines[i].Substring(0, lines[i].Length - ending.Length);
                System.Text.RegularExpressions.Match header = System.Text.RegularExpressions.Regex.Match(
                    line, @"^\s*\[([^\]]+)\]");
                if (header.Success) section = header.Groups[1].Value.Trim();
                bool other = System.Text.RegularExpressions.Regex.IsMatch(
                    section, @"^Other[0-9]+$", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
                bool tunnel = section.Equals("Tunnel", StringComparison.OrdinalIgnoreCase);
                bool settings = section.Equals("Settings", StringComparison.OrdinalIgnoreCase);
                if (!other && !tunnel && !settings) continue;
                System.Text.RegularExpressions.Match key = System.Text.RegularExpressions.Regex.Match(
                    line, @"^(\s*)(Ip|Port)(\s*=\s*)([^;]*)(.*)$",
                    System.Text.RegularExpressions.RegexOptions.IgnoreCase);
                if (!key.Success) continue;
                string name = key.Groups[2].Value;
                if (settings && !name.Equals("Port", StringComparison.OrdinalIgnoreCase)) continue;
                string replacement = name.Equals("Ip", StringComparison.OrdinalIgnoreCase) ? "127.0.0.1" :
                    settings ? "0" : "9";
                lines[i] = key.Groups[1].Value + name + key.Groups[3].Value + replacement +
                    key.Groups[5].Value + ending;
            }
            return bytes.GetBytes(string.Concat(lines));
        }
    }
}
