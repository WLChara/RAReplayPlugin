using System;
using System.IO;

namespace RAReplay.Client
{
    public static class ReplayClient
    {
        // 宿主必须先写完原生开局文件，并在进程退出或启动失败时释放返回的会话。
        public static RecordingSession PrepareRecording(string gamePath, string baseMapPath, string mapName)
        {
            string root = ReplayLaunchManifest.Root(gamePath);
            if (!File.Exists(Path.Combine(root, "RAReplayPlugin.dll")))
                throw new FileNotFoundException("游戏目录缺少 RAReplayPlugin.dll。");
            ReplayRuntimeRequest.ClearStale(root);
            ReplayLaunchManifest manifest = ReplayLaunchManifest.Create(root, baseMapPath, mapName);
            string replayDirectory = Path.Combine(root, "Replays");
            Directory.CreateDirectory(replayDirectory);
            string replayPath = Path.Combine(replayDirectory,
                DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N") + ".yrrep");
            byte[] request = ReplayRuntimeRequest.Create(
                ReplayRuntimeRequest.Record, replayPath, manifest.Serialize()).Serialize();
            ReplayRuntimeRequest.Publish(root, request);
            return new RecordingSession(root, replayPath, manifest, request);
        }

        // 地图解析属于宿主；返回的地图必须仍位于游戏目录内且内容哈希与清单一致。
        public static PlaybackSession PreparePlayback(string gamePath, string replayPath,
            string gameExecutablePath, Func<ReplayLaunchManifest, string> resolveBaseMap)
        {
            return PreparePlayback(gamePath, replayPath, gameExecutablePath, resolveBaseMap, 0);
        }

        public static PlaybackSession PreparePlayback(string gamePath, string replayPath,
            string gameExecutablePath, Func<ReplayLaunchManifest, string> resolveBaseMap, uint viewerFlags = 0)
        {
            if (resolveBaseMap == null) throw new ArgumentNullException(nameof(resolveBaseMap));
            string root = ReplayLaunchManifest.Root(gamePath);
            if (!File.Exists(Path.Combine(root, "RAReplayPlugin.dll")))
                throw new FileNotFoundException("游戏目录缺少 RAReplayPlugin.dll。");
            ReplayRuntimeRequest.ClearStale(root);
            ReplayArchive archive = ReplayArchive.Open(replayPath);
            if (!archive.HasPlayerInputEffects)
                throw new InvalidDataException("旧格式缺少玩家输入效果，请重新录制。");
            archive.ValidatePayload();
            if (!archive.MatchesCurrentHeader())
                throw new InvalidDataException("回放文件在校验后发生变化。");
            if (!ReplayLaunchManifest.EqualsHash(
                ReplayLaunchManifest.HashFile(gameExecutablePath), archive.GameExeHash))
                throw new InvalidDataException("游戏版本与回放记录时不同。");

            string map = resolveBaseMap(archive.Manifest);
            if (string.IsNullOrWhiteSpace(map)) throw new FileNotFoundException("找不到回放地图。");
            string fullMap = Path.GetFullPath(map);
            if (!IsSafeChild(root, fullMap) ||
                !ReplayLaunchManifest.EqualsHash(
                    ReplayLaunchManifest.HashFile(fullMap), archive.Manifest.BaseMapHash))
                throw new InvalidDataException("回放地图不在游戏目录内或内容不匹配。");

            return PlaybackSession.Prepare(root, archive, viewerFlags);
        }

        private static bool IsSafeChild(string root, string fullPath)
        {
            if (!fullPath.StartsWith(root, StringComparison.OrdinalIgnoreCase)) return false;
            string current = root;
            foreach (string part in fullPath.Substring(root.Length).Split(
                Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar))
            {
                if (part.Length == 0) continue;
                current = Path.Combine(current, part);
                if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0) return false;
            }
            return true;
        }
    }
}
