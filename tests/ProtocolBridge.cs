using System;
using System.IO;
using System.Linq;
using System.Text;
using RAReplay.Client;

internal static class ProtocolBridge
{
    private static readonly byte[] MapBytes = { 0x4d, 0x41, 0x50, 0x00, 0x80, 0xff };
    private static readonly byte[] SpawnIniBytes = Encoding.UTF8.GetBytes(
        "[Settings]\nSeed=305419896\n[Multi1]\nSide=2\nColor=3\nStart=4\nTeam=1\n");
    private static readonly byte[] SpawnMapBytes = Encoding.UTF8.GetBytes(
        "[Map]\nWidth=64\nHeight=64\n[Waypoints]\n0=100100\n");
    // 未完成录像样本（容器 v3）：C++ 写入 3 帧后中止（待写块写成一块），再追加下一块磁盘字节的前 10 字节。
    private const uint UnfinalizedFrameCount = 3;
    private const long UnfinalizedTailBytes = 10;
    private const long ContainerHeaderBytes = 96;

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidDataException(message);
    }

    // 容器头偏移 16 的 metadataBytes：容器 v3 中是清单段（压缩块）的磁盘字节数，不等于清单本身的长度。
    private static long ReadMetadataBytes(string path)
    {
        using (FileStream input = File.OpenRead(path))
        using (BinaryReader reader = new BinaryReader(input))
        {
            input.Position = 16;
            return reader.ReadUInt32();
        }
    }

    // 解出帧流段（文件偏移 96 + metadataBytes 起）的逻辑帧流；recover 为 true 时按未完成录像在首个无效块之前截止。
    private static byte[] ReadLogicalStream(string path, bool recover)
    {
        long start = ContainerHeaderBytes + ReadMetadataBytes(path);
        using (FileStream input = File.OpenRead(path))
        using (MemoryStream output = new MemoryStream())
        {
            new ReplayBlockStream(input, start, input.Length - start, recover).CopyTo(output);
            return output.ToArray();
        }
    }

    private static void Produce(string directory)
    {
        string game = Path.Combine(directory, "game");
        Directory.CreateDirectory(game);
        string mapPath = Path.Combine(game, "base.map");
        File.WriteAllBytes(mapPath, MapBytes);
        File.WriteAllBytes(Path.Combine(game, "spawn.ini"), SpawnIniBytes);
        File.WriteAllBytes(Path.Combine(game, "spawnmap.ini"), SpawnMapBytes);

        ReplayLaunchManifest manifest = ReplayLaunchManifest.Create(game, mapPath, "测试地图");
        byte[] serialized = manifest.Serialize();
        string replayPath = Path.Combine(directory, "bridge.yrrep");
        ReplayRuntimeRequest request = ReplayRuntimeRequest.Create(
            ReplayRuntimeRequest.Record, replayPath, serialized);
        File.WriteAllBytes(Path.Combine(directory, "manifest.bin"), serialized);
        File.WriteAllBytes(Path.Combine(directory, "request.bin"), request.Serialize());
        Console.WriteLine("C# manifest and request produced");
    }

    private static void Verify(string directory)
    {
        string game = Path.Combine(directory, "game");
        string replayPath = Path.Combine(directory, "bridge.yrrep");
        ReplayArchive archive = ReplayArchive.Open(replayPath);
        archive.ValidatePayload();
        Require(archive.ContainerVersion == 3, "C++ archive was not written as container version 3");
        Require(archive.StreamVersion == 2 && archive.HasPlayerInputEffects,
            "input effect stream version was not exposed");
        Require(!archive.IsUnfinalized && archive.RecoveredFrameCount == 0,
            "finalized archive was reported as unfinalized");
        ReplayLaunchManifest manifest = archive.Manifest;
        Require(manifest.MapName == "测试地图" &&
            manifest.BaseMapRelativePath == "base.map" &&
            manifest.FindBaseMap(game) == Path.Combine(game, "base.map"),
            "map identity was not preserved");
        Require(manifest.SpawnIniBytes.SequenceEqual(SpawnIniBytes) &&
            manifest.SpawnMapBytes.SequenceEqual(SpawnMapBytes) &&
            manifest.BaseMapHash.SequenceEqual(ReplayLaunchManifest.Hash(MapBytes)) &&
            manifest.SpawnMapHash.SequenceEqual(ReplayLaunchManifest.Hash(SpawnMapBytes)),
            "player configuration or map bytes changed");
        // 容器 v3 的帧流段是压缩块：先解出逻辑帧流，再核对其长度与内容。
        byte[] logical = ReadLogicalStream(replayPath, false);
        Require(logical.Length == 32 + 20 + 20 + 111 + 40,
            "C++ stream length differs from the two-frame sample");
        Require(archive.GameExeHash.SequenceEqual(Enumerable.Range(1, 32).Select(x => (byte)x)),
            "target hash changed across the archive boundary");

        using (MemoryStream input = new MemoryStream(logical))
        using (BinaryReader reader = new BinaryReader(input))
        {
            Require(reader.ReadUInt32() == 0x32465052 && reader.ReadUInt32() == 2 &&
                reader.ReadUInt32() == 111 && reader.ReadUInt32() == 3 &&
                reader.ReadUInt32() == 1 && reader.ReadUInt32() == 0 &&
                reader.ReadUInt32() == 6 && reader.ReadUInt32() == 0x12345678,
                "C++ session fields including random seed changed");
            Require(reader.ReadUInt32() == 0 && reader.ReadUInt32() == 0 &&
                reader.ReadUInt32() == 0 && reader.ReadUInt32() == 0 &&
                reader.ReadUInt32() == 0,
                "empty frame changed");
            Require(reader.ReadUInt32() == 1 && reader.ReadUInt32() == 0xdeadbeef &&
                reader.ReadUInt32() == 1 && reader.ReadUInt32() == 1 &&
                reader.ReadUInt32() == 1,
                "event frame header changed");
            byte[] eventBytes = reader.ReadBytes(111);
            Require(eventBytes.Length == 111 &&
                eventBytes.SequenceEqual(Enumerable.Range(0, 111).Select(x => (byte)x)) &&
                reader.ReadUInt32() == 1 && reader.ReadUInt32() == 17 &&
                reader.ReadInt32() == -23 && reader.ReadInt32() == 45 &&
                reader.ReadInt32() == -67 && reader.ReadInt32() == 8 &&
                reader.ReadInt32() == 2 && reader.ReadUInt32() == 0x12345678 &&
                reader.ReadInt32() == -9 && reader.ReadUInt32() == 1 &&
                input.Position == input.Length,
                "111-byte command order or size changed");
        }
        VerifyUnfinalized(directory);
        Console.WriteLine("C# archive open, payload validation and manifest assertions passed");
    }

    // 遗留 .partial：只看容器头 flags 识别，可恢复到最后一个有效块与最后一条完整帧，其后不完整块的 10 字节被忽略。
    private static void VerifyUnfinalized(string directory)
    {
        string partialPath = Path.Combine(directory, "bridge-unfinalized.yrrep.partial");
        long metadataBytes = ReadMetadataBytes(partialPath);
        ReplayArchive archive = ReplayArchive.Open(partialPath);
        Require(archive.IsUnfinalized, "partial archive was not recognized as unfinalized");
        Require(archive.ContainerVersion == 3, "C++ partial archive was not written as container version 3");
        Require(archive.RecoveredFrameCount == UnfinalizedFrameCount,
            "recovered frame count differs from the written frames");
        long fileBytes = new FileInfo(partialPath).Length;
        Require(archive.StreamBytes == fileBytes - ContainerHeaderBytes - metadataBytes - UnfinalizedTailBytes,
            "recovered stream length did not stop before the truncated record");
        // 帧流头 32 字节 + 空帧 20 + 事件帧 20+111+40 + 空帧 20。
        Require(ReadLogicalStream(partialPath, true).Length == 32 + 20 + 20 + 111 + 40 + 20,
            "recovered stream length differs from the three-frame sample");
        archive.ValidatePayload();
        Require(archive.StreamVersion == 2 && archive.HasPlayerInputEffects,
            "unfinalized stream version was not exposed");
        Require(archive.GameExeHash.SequenceEqual(Enumerable.Range(1, 32).Select(x => (byte)x)),
            "target hash changed in the unfinalized archive");
        Require(archive.Manifest.MapName == "测试地图" &&
            archive.Manifest.SpawnIniBytes.SequenceEqual(SpawnIniBytes),
            "manifest was not preserved in the unfinalized archive");
    }

    private static int Main(string[] args)
    {
        try
        {
            if (args.Length != 2) throw new ArgumentException("mode and directory required");
            if (args[0] == "produce") Produce(args[1]);
            else if (args[0] == "verify") Verify(args[1]);
            else throw new ArgumentException("unknown mode");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }
}
