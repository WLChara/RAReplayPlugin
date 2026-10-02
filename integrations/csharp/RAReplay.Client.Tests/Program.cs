using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using RAReplay.Client;

internal static class Program
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }

    private static byte[] Hash(byte[] bytes)
    {
        using (SHA256 sha = SHA256.Create()) return sha.ComputeHash(bytes);
    }

    private static void WriteArchive(string path, byte[] exe, byte[] metadata)
    {
        byte[] stream;
        using (MemoryStream output = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(0x32465052u);
            writer.Write(2u);
            writer.Write(111u);
            writer.Write(3u);
            writer.Write(1u);
            writer.Write(0u);
            writer.Write(6u);
            writer.Write(0x12345678u);
            stream = output.ToArray();
        }
        using (MemoryStream output = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(Encoding.ASCII.GetBytes("RAREPL2\0"));
            writer.Write(2u);
            writer.Write(96u);
            writer.Write((uint)metadata.Length);
            writer.Write(1u);
            writer.Write((ulong)stream.Length);
            writer.Write(Hash(exe));
            writer.Write(Hash(metadata.Concat(stream).ToArray()));
            writer.Write(metadata);
            writer.Write(stream);
            File.WriteAllBytes(path, output.ToArray());
        }
    }

    private static void Run(string root)
    {
        Directory.CreateDirectory(root);
        byte[] map = Encoding.ASCII.GetBytes("synthetic map");
        byte[] exe = Encoding.ASCII.GetBytes("synthetic exe");
        byte[] ini = Encoding.ASCII.GetBytes("[Settings]\nSeed=42\nPort=1234\n[Other1]\nIp=10.0.0.1\n");
        byte[] spawnMap = Encoding.ASCII.GetBytes("[Map]\nWidth=64\n");
        string mapPath = Path.Combine(root, "base.map");
        string exePath = Path.Combine(root, "gamemd.exe");
        string iniPath = Path.Combine(root, "spawn.ini");
        string spawnMapPath = Path.Combine(root, "spawnmap.ini");
        string marker = Path.Combine(root, "rareplay-runtime.bin");
        File.WriteAllBytes(mapPath, map);
        File.WriteAllBytes(exePath, exe);
        File.WriteAllBytes(Path.Combine(root, "RAReplayPlugin.dll"), new byte[0]);
        File.WriteAllBytes(iniPath, ini);
        File.WriteAllBytes(spawnMapPath, spawnMap);

        ReplayLaunchManifest manifest = ReplayLaunchManifest.Create(root, mapPath, "测试地图");
        byte[] metadata = manifest.Serialize();
        Check(ReplayLaunchManifest.Deserialize(metadata).SpawnIniBytes.SequenceEqual(ini), "manifest bytes");
        string archivePath = Path.Combine(root, "synthetic.yrrep");
        WriteArchive(archivePath, exe, metadata);
        ReplayArchive archive = ReplayArchive.Open(archivePath);
        archive.ValidatePayload();
        Check(archive.Manifest.MapName == "测试地图", "archive metadata");

        using (RecordingSession recording = ReplayClient.PrepareRecording(root, mapPath, "测试地图"))
        {
            ReplayRuntimeRequest request = ReplayRuntimeRequest.Deserialize(File.ReadAllBytes(marker));
            Check(request.Mode == ReplayRuntimeRequest.Record &&
                request.ReplayPath == recording.ReplayPath &&
                request.ManifestBytes.SequenceEqual(recording.Manifest.Serialize()), "record request");
        }
        Check(!File.Exists(marker), "record cleanup");

        using (PlaybackSession playback = ReplayClient.PreparePlayback(
            root, archivePath, exePath, loaded => loaded.FindBaseMap(root)))
        {
            ReplayRuntimeRequest request = ReplayRuntimeRequest.Deserialize(File.ReadAllBytes(marker));
            Check(request.Mode == ReplayRuntimeRequest.Play && request.ManifestBytes.Length == 0,
                "play request");
            Check(Directory.GetFiles(root, "rareplay-viewer-*.bin").Length == 0,
                "legacy four-argument host must not advertise restart");
            Check(!File.ReadAllBytes(iniPath).SequenceEqual(ini), "playback endpoints");
            Check(File.ReadAllBytes(spawnMapPath).SequenceEqual(spawnMap), "playback spawn map");
        }
        Check(!File.Exists(marker) && File.ReadAllBytes(iniPath).SequenceEqual(ini) &&
            File.ReadAllBytes(spawnMapPath).SequenceEqual(spawnMap), "dispose restores originals");

        File.Delete(iniPath);
        File.Delete(spawnMapPath);
        using (PlaybackSession playback = ReplayClient.PreparePlayback(
            root, archivePath, exePath, loaded => loaded.FindBaseMap(root)))
        {
            Check(File.Exists(iniPath) && File.Exists(spawnMapPath), "prepare missing files");
        }
        Check(!File.Exists(iniPath) && !File.Exists(spawnMapPath), "dispose restores absence");

        File.WriteAllBytes(iniPath, ini);
        File.WriteAllBytes(spawnMapPath, spawnMap);
        byte[] occupied = ReplayRuntimeRequest.Create(
            ReplayRuntimeRequest.Play, archivePath, new byte[0]).Serialize();
        File.WriteAllBytes(marker, occupied);
        bool rejected = false;
        try
        {
            ReplayClient.PreparePlayback(root, archivePath, exePath, loaded => loaded.FindBaseMap(root));
        }
        catch (IOException) { rejected = true; }
        Check(rejected, "occupied request must reject preparation");
        Check(File.ReadAllBytes(marker).SequenceEqual(occupied) &&
            File.ReadAllBytes(iniPath).SequenceEqual(ini) &&
            File.ReadAllBytes(spawnMapPath).SequenceEqual(spawnMap), "prepare failure rollback");
        File.Delete(marker);

        byte[] id = Enumerable.Range(0, 16).Select(value => (byte)value).ToArray();
        using (ReplayViewerChannel channel = ReplayViewerChannel.Create(root, id))
        {
            Check(Path.GetFileName(channel.ChannelPath) ==
                "rareplay-viewer-000102030405060708090a0b0c0d0e0f.bin", "raw ID filename");
            byte[] ready = File.ReadAllBytes(channel.ChannelPath);
            Check(ready.Length == 32 && BitConverter.ToUInt32(ready, 0) == 0x43565052u &&
                BitConverter.ToUInt32(ready, 4) == 1u && ready.Skip(16).SequenceEqual(id), "RPVC layout");
            uint flags;
            Check(!channel.TryConsumeRestart(out flags), "Ready must not restart");
            byte[] invalid = (byte[])ready.Clone(); invalid[16] ^= 1;
            File.WriteAllBytes(channel.ChannelPath, invalid);
            Check(!channel.TryConsumeRestart(out flags), "wrong ID rejected");
            bool deleteRejected = false;
            try { channel.Dispose(); } catch (IOException) { deleteRejected = true; }
            Check(deleteRejected && File.Exists(channel.ChannelPath), "foreign ID not deleted");
            File.WriteAllBytes(channel.ChannelPath, new byte[31]);
            Check(!channel.TryConsumeRestart(out flags), "bad length rejected");
            invalid = (byte[])ready.Clone(); invalid[8] = 1; invalid[12] = 2;
            File.WriteAllBytes(channel.ChannelPath, invalid);
            Check(!channel.TryConsumeRestart(out flags), "unknown flags rejected");
            invalid[12] = 1;
            File.WriteAllBytes(channel.ChannelPath, invalid);
            Check(channel.TryConsumeRestart(out flags) && flags == ReplayViewerChannel.HalfShroud,
                "matching Restart carries shroud flag");
            Check(!channel.TryConsumeRestart(out flags), "Restart consumed only once");
        }

        int starts = 0;
        System.Collections.Generic.Queue<Action> pending = new System.Collections.Generic.Queue<Action>();
        HostReplayAdapter host = new HostReplayAdapter(() => starts++, callback => pending.Enqueue(callback), () => false);
        host.OnReplaySelected(root, archivePath, exePath);
        host.OnGameProcessExited();
        Check(starts == 1 && pending.Count == 0, "ordinary exit must not queue restart");
        host.OnReplaySelected(root, archivePath, exePath);
        ReplayRuntimeRequest playRequest = ReplayRuntimeRequest.Deserialize(File.ReadAllBytes(marker));
        string control = Path.Combine(root, "rareplay-viewer-" +
            string.Concat(playRequest.RequestId.Select(value => value.ToString("x2"))) + ".bin");
        byte[] restartBytes = File.ReadAllBytes(control); restartBytes[8] = 1; restartBytes[12] = 1;
        File.WriteAllBytes(control, restartBytes);
        host.OnGameProcessExited();
        Check(pending.Count == 1 && !File.Exists(marker) && !File.Exists(control) &&
            File.ReadAllBytes(iniPath).SequenceEqual(ini), "cleanup precedes queued restart");
        pending.Dequeue()();
        playRequest = ReplayRuntimeRequest.Deserialize(File.ReadAllBytes(marker));
        control = Path.Combine(root, "rareplay-viewer-" +
            string.Concat(playRequest.RequestId.Select(value => value.ToString("x2"))) + ".bin");
        Check(starts == 3 && BitConverter.ToUInt32(File.ReadAllBytes(control), 12) == 1,
            "UI restart creates new session and preserves shroud");
        restartBytes = File.ReadAllBytes(control); restartBytes[8] = 1;
        File.WriteAllBytes(control, restartBytes);
        using (FileStream occupiedIni = new FileStream(iniPath, FileMode.Open, FileAccess.Read, FileShare.None))
        {
            bool cleanupFailed = false;
            try { host.OnGameProcessExited(); } catch (IOException) { cleanupFailed = true; }
            Check(cleanupFailed && pending.Count == 0 && starts == 3, "cleanup failure never reopens");
        }
        host.OnGameProcessExited();
        Check(pending.Count == 0, "cleanup retry cannot replay consumed restart");
    }

    private static int Main()
    {
        string root = Path.Combine(Path.GetTempPath(), "RAReplayClientTests-" + Guid.NewGuid().ToString("N"));
        try
        {
            Run(root);
            Console.WriteLine("RAReplay.Client 合成协议与重播生命周期检查通过");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, true);
        }
    }
}
