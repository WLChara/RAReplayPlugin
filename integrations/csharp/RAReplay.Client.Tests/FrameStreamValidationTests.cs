using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
#if MODIFIED_ARCHIVE_TESTS
using DTAClient.Domain.Replay;
#else
using RAReplay.Client;
#endif

internal static class FrameStreamValidationTests
{
    private static int checks;

    private static void Check(bool condition, string name)
    {
        if (!condition) throw new Exception(name);
        checks++;
    }

    private static byte[] Hash(byte[] value)
    {
        using (SHA256 sha = SHA256.Create()) return sha.ComputeHash(value);
    }

    private static void WriteArchive(string path, byte[] metadata, byte[] frames)
    {
        using (FileStream output = File.Create(path))
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(Encoding.ASCII.GetBytes("RAREPL2\0"));
            writer.Write(2u);
            writer.Write(96u);
            writer.Write((uint)metadata.Length);
            writer.Write(1u);
            writer.Write((ulong)frames.Length);
            writer.Write(new byte[32]);
            writer.Write(Hash(metadata.Concat(frames).ToArray()));
            writer.Write(metadata);
            writer.Write(frames);
        }
    }

    private static byte[] CreateStream(uint version, params byte[][] frames)
    {
        using (MemoryStream output = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(0x32465052u);
            writer.Write(version);
            writer.Write(111u);
            writer.Write(3u);
            writer.Write(1u);
            writer.Write(0u);
            writer.Write(6u);
            writer.Write(0x12345678u);
            foreach (byte[] frame in frames) writer.Write(frame);
            return output.ToArray();
        }
    }

    private static byte[] CreateFrame(uint version, uint number, uint flags, params uint[] members)
    {
        using (MemoryStream output = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(number);
            writer.Write(0u);
            writer.Write(0u);
            writer.Write(flags);
            if (version >= 2) writer.Write(0u);
            if (version == 3) writer.Write((uint)(members.Length / 3));
            foreach (uint member in members) writer.Write(member);
            return output.ToArray();
        }
    }

    private static byte[] Change(byte[] source, int offset, uint value)
    {
        byte[] changed = (byte[])source.Clone();
        Array.Copy(BitConverter.GetBytes(value), 0, changed, offset, 4);
        return changed;
    }

    private static void Accept(string path, byte[] metadata, byte[] frames, uint version, string name)
    {
        WriteArchive(path, metadata, frames);
        ReplayArchive archive = ReplayArchive.Open(path);
        archive.ValidatePayload();
        Check(archive.StreamVersion == version && archive.HasPlayerInputEffects == (version >= 2) &&
            archive.HasRecordedGroups == (version == 3), name);
    }

    private static void Reject(string path, byte[] metadata, byte[] frames, string name)
    {
        WriteArchive(path, metadata, frames);
        bool rejected = false;
        try { ReplayArchive.Open(path).ValidatePayload(); }
        catch (InvalidDataException) { rejected = true; }
        catch (EndOfStreamException) { rejected = true; }
        Check(rejected, name);
    }

    public static void Run(string root, string fixtureDirectory)
    {
        string mapPath = Path.Combine(root, "frame-test.map");
        File.WriteAllText(mapPath, "[Map]\nWidth=64\n", Encoding.UTF8);
        File.WriteAllText(Path.Combine(root, "spawn.ini"), "[Settings]\nSeed=42\n", Encoding.UTF8);
        File.WriteAllText(Path.Combine(root, "spawnmap.ini"), "[Map]\nWidth=64\n", Encoding.UTF8);
        byte[] metadata = ReplayLaunchManifest.Create(root, mapPath, "帧流测试").Serialize();
        string archivePath = Path.Combine(root, "frame-validation.yrrep");
        Accept(archivePath, metadata, CreateStream(1), 1, "V1 零帧兼容");
        Accept(archivePath, metadata, CreateStream(2), 2, "V2 零帧兼容");
        Accept(archivePath, metadata, CreateStream(1, CreateFrame(1, 0, 1)), 1, "V1 帧兼容");
        Accept(archivePath, metadata, CreateStream(2, CreateFrame(2, 0, 1)), 2, "V2 帧兼容");
        byte[] initial = CreateFrame(3, 0, 2);
        byte[] grouped = CreateFrame(3, 1, 3, 0, 101, 52, 0, 102, 52, 9, 202, 52);
        byte[] changed = CreateFrame(3, 2, 2, 1, 101, 52, 9, 202, 52);
        byte[] retained = CreateFrame(3, 3, 0);
        byte[] cleared = CreateFrame(3, 4, 2);
        byte[] valid = CreateStream(3, initial, grouped, changed, retained, cleared);
        Accept(archivePath, metadata, CreateStream(3, initial), 3, "V3 初始空快照");
        Accept(archivePath, metadata, valid, 3, "V3 改组、保持与清空");
        Accept(archivePath, metadata, CreateStream(3, CreateFrame(3, 0, 2, 9, uint.MaxValue, 52)),
            3, "V3 最大对象编号");
        uint[] maximumMembers = new uint[16384 * 3];
        for (int i = 0; i < 16384; i++)
        {
            maximumMembers[i * 3 + 1] = (uint)i + 1;
            maximumMembers[i * 3 + 2] = 52;
        }
        Accept(archivePath, metadata, CreateStream(3, CreateFrame(3, 0, 2, maximumMembers)),
            3, "V3 最大成员数量");
        Reject(archivePath, metadata, CreateStream(3), "V3 零帧");
        Reject(archivePath, metadata, CreateStream(3, CreateFrame(3, 0, 0)), "V3 缺首帧快照");
        Reject(archivePath, metadata, CreateStream(3, CreateFrame(3, 0, 0, 0, 101, 52)), "V3 无快照却有成员");
        Reject(archivePath, metadata, CreateStream(3, initial, CreateFrame(3, 1, 0, 0, 101, 52)), "V3 后帧无快照却有成员");
        Reject(archivePath, metadata, Change(valid, 44, 6), "V3 保留标志");
        Reject(archivePath, metadata, Change(valid, 52, 16385), "V3 成员超限");
        Reject(archivePath, metadata, Change(valid, 40, 16385), "V3 事件超限");
        Reject(archivePath, metadata, Change(valid, 48, 4097), "V3 输入效果超限");
        Reject(archivePath, metadata, Change(valid, 40, uint.MaxValue), "V3 事件计数溢出");
        Reject(archivePath, metadata, Change(valid, 48, uint.MaxValue), "V3 效果计数溢出");
        Reject(archivePath, metadata, Change(valid, 52, uint.MaxValue), "V3 成员计数溢出");
        Reject(archivePath, metadata, Change(valid, 32, 1), "V3 非零起始帧");
        Reject(archivePath, metadata, Change(valid, 56, 0), "V3 后续帧重复");
        Reject(archivePath, metadata, Change(valid, 20, 1), "流头非零起始帧");
        Reject(archivePath, metadata, Change(valid, 8, 110), "流头事件尺寸");
        Reject(archivePath, metadata, Change(valid, 4, 4), "未知版本");
        Reject(archivePath, metadata, Change(valid, 0, 0), "错误魔数");
        Reject(archivePath, metadata, CreateStream(3, CreateFrame(3, 0, 2, 10, 101, 52)), "V3 组编号越界");
        Reject(archivePath, metadata, CreateStream(3, CreateFrame(3, 0, 2, 0, 0, 52)), "V3 对象编号零");
        Reject(archivePath, metadata, CreateStream(3, CreateFrame(3, 0, 2, 0, 101, 51)), "V3 对象类型错误");
        Reject(archivePath, metadata, CreateStream(3, CreateFrame(3, 0, 2, 0, 101, 52, 0, 101, 52)), "V3 同组重复");
        Reject(archivePath, metadata, CreateStream(3, CreateFrame(3, 0, 2, 0, 101, 52, 1, 101, 52)), "V3 跨组重复");
        Reject(archivePath, metadata, CreateStream(3, CreateFrame(3, 0, 2, 1, 101, 52, 0, 102, 52)), "V3 组排序错误");
        Reject(archivePath, metadata, CreateStream(3, CreateFrame(3, 0, 2, 0, 102, 52, 0, 101, 52)), "V3 成员排序错误");
        Reject(archivePath, metadata, CreateStream(1, CreateFrame(1, 0, 2)), "V1 拒绝组标志");
        Reject(archivePath, metadata, CreateStream(2, CreateFrame(2, 0, 2)), "V2 拒绝组标志");
        Reject(archivePath, metadata, CreateStream(1, CreateFrame(1, 0, 0)).Take(47).ToArray(), "V1 帧头截断");
        Reject(archivePath, metadata, CreateStream(2, CreateFrame(2, 0, 0)).Take(51).ToArray(), "V2 帧头截断");
        // 仅在完整帧边界结束的前缀合法；逐字节截断覆盖帧头及成员字段。
        int[] completeLengths = { 56, 116, 164, 188 };
        for (int length = 32; length < valid.Length; length++)
            if (!completeLengths.Contains(length))
                Reject(archivePath, metadata, valid.Take(length).ToArray(), "V3 截断位置 " + length);
        if (!string.IsNullOrEmpty(fixtureDirectory))
        {
            byte[] cppV2 = File.ReadAllBytes(Path.Combine(fixtureDirectory, "rpf2-groups-v2.bin"));
            byte[] cppV3 = File.ReadAllBytes(Path.Combine(fixtureDirectory, "rpf2-groups-v3.bin"));
            Check(cppV2.Length == 223 && cppV3.Length == 363, "实际 C++ 样本长度");
            Accept(archivePath, metadata, cppV2, 2, "实际 C++ V2 事件与效果");
            Accept(archivePath, metadata, cppV3, 3, "实际 C++ V3 事件、效果与组历史");
            Reject(archivePath, metadata, Change(cppV3, 56 + 24 + 111, 2), "V3 效果类型错误");
            Reject(archivePath, metadata, Change(cppV3, 56 + 24 + 111 + 36, 2), "V3 效果布尔字段错误");
            int[] cppCompleteLengths = { 56, 267, 315, 339 };
            for (int length = 32; length < cppV3.Length; length++)
                if (!cppCompleteLengths.Contains(length))
                    Reject(archivePath, metadata, cppV3.Take(length).ToArray(), "实际 C++ V3 截断位置 " + length);
            Console.WriteLine("实际 C++ RPF2 样本经 RAREPL2 V2 包装后校验通过");
        }
        Console.WriteLine("帧流校验检查通过：" + checks);
    }

#if FRAME_STREAM_TEST_RUNNER
    private static int Main(string[] args)
    {
        string root = Path.Combine(Path.GetTempPath(), "RAReplayFrameTests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try { Run(root, args.Length == 0 ? null : args[0]); return 0; }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
        finally { Directory.Delete(root, true); }
    }
#endif
}
