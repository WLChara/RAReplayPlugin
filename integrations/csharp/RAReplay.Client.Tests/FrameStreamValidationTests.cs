using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using RAReplay.Client;

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
        return CreateModeStream(version, 3, frames);
    }

    // 帧流头可指定 gameMode：0（战役）与 5（遭遇战）为单机，其余为联机；版本 4 起的游戏速度规则依此区分。
    private static byte[] CreateModeStream(uint version, uint gameMode, params byte[][] frames)
    {
        using (MemoryStream output = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(0x32465052u);
            writer.Write(version);
            writer.Write(111u);
            writer.Write(gameMode);
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
        // 完成文件不是未完成录像：恢复相关属性保持默认，帧流长度为头中记录的长度；gameMode 取自帧流头。
        Check(archive.StreamVersion == version && archive.HasPlayerInputEffects == (version >= 2) &&
            archive.HasRecordedGroups == (version >= 3) && archive.HasGameSpeedRecords == (version >= 4) &&
            archive.StreamGameMode == BitConverter.ToUInt32(frames, 12) && !archive.IsUnfinalized &&
            archive.RecoveredFrameCount == 0 && archive.StreamBytes == frames.Length, name);
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

    // 写任意容器头：可单独覆盖 flags、streamBytes、内容哈希与 metadataBytes，用于构造未完成录像及其非法变体。
    private static void WriteContainer(string path, byte[] metadata, byte[] frames, uint flags,
        ulong streamBytes, byte[] contentHash, uint metadataBytes)
    {
        using (FileStream output = File.Create(path))
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(Encoding.ASCII.GetBytes("RAREPL2\0"));
            writer.Write(2u);
            writer.Write(96u);
            writer.Write(metadataBytes);
            writer.Write(flags);
            writer.Write(streamBytes);
            writer.Write(Enumerable.Repeat((byte)0x11, 32).ToArray());
            writer.Write(contentHash);
            writer.Write(metadata);
            writer.Write(frames);
        }
    }

    // 录制中止遗留的容器：flags=0、streamBytes=0、内容哈希全零，帧流为实际已追加的内容。
    private static void WritePartial(string path, byte[] metadata, byte[] frames)
    {
        WriteContainer(path, metadata, frames, 0, 0, new byte[32], (uint)metadata.Length);
    }

    private static void RejectOpen(string path, string name)
    {
        bool rejected = false;
        try { ReplayArchive.Open(path); }
        catch (InvalidDataException) { rejected = true; }
        Check(rejected, name);
    }

    private static void RejectPartial(string path, byte[] metadata, byte[] frames, string name)
    {
        WritePartial(path, metadata, frames);
        RejectOpen(path, name);
    }

    // 未完成录像应被恢复为 frameCount 条帧、streamBytes 字节（含 32 字节帧流头），并通过逐帧校验。
    private static ReplayArchive Recover(string path, byte[] metadata, byte[] frames, uint version,
        int frameCount, long streamBytes, string name)
    {
        WritePartial(path, metadata, frames);
        ReplayArchive archive = ReplayArchive.Open(path);
        archive.ValidatePayload();
        Check(archive.IsUnfinalized && archive.StreamVersion == version &&
            archive.StreamGameMode == BitConverter.ToUInt32(frames, 12) &&
            archive.RecoveredFrameCount == (uint)frameCount && archive.StreamBytes == streamBytes &&
            archive.MatchesCurrentHeader(), name);
        return archive;
    }

    private static byte[] CreateEffect(uint kind = 1, uint reverse = 0)
    {
        using (MemoryStream output = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(kind);
            writer.Write(17u);
            writer.Write(-23);
            writer.Write(45);
            writer.Write(-67);
            writer.Write(8);
            writer.Write(2);
            writer.Write(0x12345678u);
            writer.Write(-9);
            writer.Write(reverse);
            return output.ToArray();
        }
    }

    // 带事件、输入效果与编队成员的帧记录；版本 1 没有效果计数，版本 2 没有成员计数。
    private static byte[] CreateFullFrame(uint version, uint number, uint flags, uint eventCount,
        uint effectCount, params uint[] members)
    {
        using (MemoryStream output = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(number);
            writer.Write(0x01020304u);
            writer.Write(eventCount);
            writer.Write(flags);
            if (version >= 2) writer.Write(effectCount);
            if (version == 3) writer.Write((uint)(members.Length / 3));
            for (uint i = 0; i < eventCount; i++)
            {
                byte[] gameEvent = new byte[111];
                gameEvent[0] = 0xA1;
                gameEvent[110] = 0xB2;
                writer.Write(gameEvent);
            }
            if (version >= 2)
                for (uint i = 0; i < effectCount; i++) writer.Write(CreateEffect());
            foreach (uint member in members) writer.Write(member);
            return output.ToArray();
        }
    }

    // 版本 4 帧记录：24 字节帧头；flags 含 bit2 时紧接 4 字节游戏速度，其后依次为事件、输入效果与编队成员。
    private static byte[] CreateV4Frame(uint number, uint flags, uint speed, uint eventCount, uint effectCount,
        params uint[] members)
    {
        using (MemoryStream output = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(number);
            writer.Write(0x01020304u);
            writer.Write(eventCount);
            writer.Write(flags);
            writer.Write(effectCount);
            writer.Write((uint)(members.Length / 3));
            if ((flags & 4u) != 0) writer.Write(speed);
            for (uint i = 0; i < eventCount; i++)
            {
                byte[] gameEvent = new byte[111];
                gameEvent[0] = 0xA1;
                gameEvent[110] = 0xB2;
                writer.Write(gameEvent);
            }
            for (uint i = 0; i < effectCount; i++) writer.Write(CreateEffect());
            foreach (uint member in members) writer.Write(member);
            return output.ToArray();
        }
    }

    // 每条帧记录结束处的流内偏移（含 32 字节帧流头）。
    private static int[] Boundaries(params byte[][] frames)
    {
        int[] ends = new int[frames.Length];
        int offset = 32;
        for (int i = 0; i < frames.Length; i++)
        {
            offset += frames[i].Length;
            ends[i] = offset;
        }
        return ends;
    }

    // 逐字节截断：恢复结果必须是不超过截断点的最后一个完整帧边界；没有完整帧则拒绝。
    private static void CheckPrefixes(string path, byte[] metadata, byte[] stream, uint version,
        int[] ends, string name)
    {
        Check(ends[ends.Length - 1] == stream.Length, name + " 边界与流长度一致");
        for (int length = 0; length <= stream.Length; length++)
        {
            int last = -1;
            for (int i = 0; i < ends.Length; i++)
                if (ends[i] <= length) last = i;
            byte[] prefix = stream.Take(length).ToArray();
            if (last < 0) RejectPartial(path, metadata, prefix, name + " 无完整帧 " + length);
            else Recover(path, metadata, prefix, version, last + 1, ends[last], name + " 截断位置 " + length);
        }
    }

    // 未完成录像（容器头 flags==0）：从帧 0 起恢复最长的完整有效连续帧，其后内容忽略。
    private static void RunUnfinalized(string root, byte[] metadata, string fixtureDirectory)
    {
        string path = Path.Combine(root, "unfinalized.yrrep.partial");
        byte[] initial = CreateFrame(3, 0, 2);
        byte[] grouped = CreateFrame(3, 1, 3, 0, 101, 52, 0, 102, 52, 9, 202, 52);
        byte[] changed = CreateFrame(3, 2, 2, 1, 101, 52, 9, 202, 52);
        byte[] retained = CreateFrame(3, 3, 0);
        byte[] cleared = CreateFrame(3, 4, 2);
        byte[] valid = CreateStream(3, initial, grouped, changed, retained, cleared);
        int[] validEnds = Boundaries(initial, grouped, changed, retained, cleared);

        // 完整的未完成录像按全长恢复；带 .partial 后缀与否不影响（是否未完成只看 flags）。
        Recover(path, metadata, valid, 3, 5, valid.Length, "V3 完整帧流作为未完成录像");
        CheckPrefixes(path, metadata, valid, 3, validEnds, "V3 未完成");

        // 事件、输入效果与编队成员的中途截断。
        byte[] richA = CreateFullFrame(3, 0, 2, 2, 1, 0, 101, 52);
        byte[] richB = CreateFullFrame(3, 1, 0, 1, 2);
        byte[] richC = CreateFullFrame(3, 2, 2, 0, 0, 0, 101, 52, 1, 103, 52, 9, 200, 52);
        byte[] rich = CreateStream(3, richA, richB, richC);
        int[] richEnds = Boundaries(richA, richB, richC);
        Recover(path, metadata, rich, 3, 3, rich.Length, "V3 含事件、效果与成员的完整恢复");
        CheckPrefixes(path, metadata, rich, 3, richEnds, "V3 事件效果成员");
        Recover(path, metadata, rich.Take(richEnds[0] + 10).ToArray(), 3, 1, richEnds[0], "V3 帧头中途截断");
        Recover(path, metadata, rich.Take(richEnds[0] + 24 + 50).ToArray(), 3, 1, richEnds[0], "V3 事件中途截断");
        Recover(path, metadata, rich.Take(richEnds[0] + 24 + 111 + 20).ToArray(), 3, 1, richEnds[0],
            "V3 首个输入效果中途截断");
        Recover(path, metadata, rich.Take(richEnds[0] + 24 + 111 + 40 + 20).ToArray(), 3, 1, richEnds[0],
            "V3 第二个输入效果中途截断");
        Recover(path, metadata, rich.Take(richEnds[1] + 24 + 12 + 5).ToArray(), 3, 2, richEnds[1],
            "V3 编队成员中途截断");
        Recover(path, metadata, rich.Take(richEnds[2] - 1).ToArray(), 3, 2, richEnds[1],
            "V3 最后一个成员缺一字节");

        // 版本 2 与版本 1：帧头更短，没有编队成员。
        byte[] v2A = CreateFullFrame(2, 0, 1, 1, 1);
        byte[] v2B = CreateFullFrame(2, 1, 0, 0, 0);
        byte[] v2C = CreateFullFrame(2, 2, 1, 2, 2);
        byte[] v2 = CreateStream(2, v2A, v2B, v2C);
        int[] v2Ends = Boundaries(v2A, v2B, v2C);
        Recover(path, metadata, v2, 2, 3, v2.Length, "V2 完整恢复");
        CheckPrefixes(path, metadata, v2, 2, v2Ends, "V2 未完成");
        Recover(path, metadata, v2.Take(v2Ends[0] + 10).ToArray(), 2, 1, v2Ends[0], "V2 帧头中途截断");
        Recover(path, metadata, v2.Take(v2Ends[1] + 20 + 2 * 111 + 20).ToArray(), 2, 2, v2Ends[1],
            "V2 效果中途截断");
        Recover(path, metadata, v2.Take(v2Ends[1] + 20 + 111 + 30).ToArray(), 2, 2, v2Ends[1],
            "V2 事件中途截断");
        byte[] v1A = CreateFullFrame(1, 0, 1, 1, 0);
        byte[] v1B = CreateFullFrame(1, 1, 0, 0, 0);
        byte[] v1 = CreateStream(1, v1A, v1B);
        Recover(path, metadata, v1, 1, 2, v1.Length, "V1 完整恢复");
        CheckPrefixes(path, metadata, v1, 1, Boundaries(v1A, v1B), "V1 未完成");

        // 全零尾部：帧号不连续，在尾部之前截止。
        Recover(path, metadata, valid.Concat(new byte[64]).ToArray(), 3, 5, valid.Length, "V3 全零尾部");
        Recover(path, metadata, valid.Concat(new byte[3]).ToArray(), 3, 5, valid.Length, "V3 三字节零尾部");
        Recover(path, metadata, valid.Take(validEnds[1]).Concat(new byte[24]).ToArray(), 3, 2, validEnds[1],
            "V3 一帧长度的全零尾部");
        Recover(path, metadata, v2.Concat(new byte[200]).ToArray(), 2, 3, v2.Length, "V2 全零尾部");
        RejectPartial(path, metadata, CreateStream(3).Concat(new byte[100]).ToArray(),
            "V3 帧流头后只有全零（首帧缺编队快照）");
        // 按共同规则，V1/V2 帧流头之后的零字节恰是帧 0 的完整有效记录；其后的零因帧号不连续而截止。
        Recover(path, metadata, CreateStream(2).Concat(new byte[100]).ToArray(), 2, 1, 32 + 20,
            "V2 帧流头后全零按共同规则恢复一条空帧");

        // 完整但无效的记录：在它之前截止，其后即使有效的记录也被忽略。
        Recover(path, metadata, CreateStream(3, initial, grouped, CreateFrame(3, 3, 0), CreateFrame(3, 4, 0)), 3, 2,
            validEnds[1], "V3 中间帧号跳跃");
        Recover(path, metadata, CreateStream(3, initial, CreateFrame(3, 0, 0), CreateFrame(3, 1, 0)), 3, 1,
            validEnds[0], "V3 帧号重复");
        Recover(path, metadata, CreateStream(3, initial, CreateFrame(3, 1, 4), CreateFrame(3, 2, 0)), 3, 1,
            validEnds[0], "V3 中间帧保留标志位");
        Recover(path, metadata, CreateStream(3, initial, CreateFrame(3, 1, 0, 0, 101, 52), CreateFrame(3, 2, 0)),
            3, 1, validEnds[0], "V3 中间帧无快照却有成员");
        Recover(path, metadata, CreateStream(3, initial, CreateFrame(3, 1, 2, 0, 102, 52, 0, 101, 52),
            CreateFrame(3, 2, 0)), 3, 1, validEnds[0], "V3 中间帧成员排序错误");
        Recover(path, metadata, CreateStream(3, initial, CreateFrame(3, 1, 2, 0, 101, 52, 1, 101, 52),
            CreateFrame(3, 2, 0)), 3, 1, validEnds[0], "V3 中间帧成员对象重复");
        Recover(path, metadata, CreateStream(3, initial, CreateFrame(3, 1, 2, 0, 0, 52), CreateFrame(3, 2, 0)),
            3, 1, validEnds[0], "V3 中间帧成员对象编号为零");
        Recover(path, metadata, CreateStream(3, initial, CreateFrame(3, 1, 2, 10, 101, 52), CreateFrame(3, 2, 0)),
            3, 1, validEnds[0], "V3 中间帧组编号越界");
        Recover(path, metadata, CreateStream(3, initial, CreateFrame(3, 1, 2, 0, 101, 51), CreateFrame(3, 2, 0)),
            3, 1, validEnds[0], "V3 中间帧成员类型错误");
        Recover(path, metadata, CreateStream(3, initial, Change(CreateFullFrame(3, 1, 0, 0, 1), 24, 2),
            CreateFrame(3, 2, 0)), 3, 1, validEnds[0], "V3 中间帧效果类型错误");
        Recover(path, metadata, CreateStream(3, initial, Change(CreateFullFrame(3, 1, 0, 0, 1), 24 + 36, 2),
            CreateFrame(3, 2, 0)), 3, 1, validEnds[0], "V3 中间帧效果布尔字段错误");
        Recover(path, metadata, CreateStream(3, initial, Change(CreateFrame(3, 1, 0), 8, 16385),
            CreateFrame(3, 2, 0)), 3, 1, validEnds[0], "V3 中间帧事件数量超限");
        Recover(path, metadata, CreateStream(3, initial, Change(CreateFrame(3, 1, 0), 16, 4097),
            CreateFrame(3, 2, 0)), 3, 1, validEnds[0], "V3 中间帧效果数量超限");
        Recover(path, metadata, CreateStream(3, initial, Change(CreateFrame(3, 1, 2), 20, 16385),
            CreateFrame(3, 2, 0)), 3, 1, validEnds[0], "V3 中间帧成员数量超限");
        byte[] v2First = CreateFullFrame(2, 0, 1, 1, 1);
        int v2FirstEnd = 32 + v2First.Length;
        Recover(path, metadata, CreateStream(2, v2First, CreateFrame(2, 2, 0), CreateFrame(2, 1, 0)), 2, 1,
            v2FirstEnd, "V2 中间帧号跳跃");
        Recover(path, metadata, CreateStream(2, v2First, CreateFrame(2, 1, 2), CreateFrame(2, 2, 0)), 2, 1,
            v2FirstEnd, "V2 中间帧使用组标志");
        Recover(path, metadata, CreateStream(2, v2First, Change(CreateFullFrame(2, 1, 0, 0, 1), 20, 2),
            CreateFrame(2, 2, 0)), 2, 1, v2FirstEnd, "V2 中间帧效果类型错误");
        Recover(path, metadata, CreateStream(2, v2First, Change(CreateFullFrame(2, 1, 0, 0, 1), 20 + 36, 2),
            CreateFrame(2, 2, 0)), 2, 1, v2FirstEnd, "V2 中间帧效果布尔字段错误");

        // 没有完整有效帧：拒绝。
        RejectPartial(path, metadata, CreateStream(1), "V1 帧流只有头");
        RejectPartial(path, metadata, CreateStream(2), "V2 帧流只有头");
        RejectPartial(path, metadata, CreateStream(3), "V3 帧流只有头");
        RejectPartial(path, metadata, new byte[0], "没有帧流头");
        RejectPartial(path, metadata, CreateStream(3).Take(31).ToArray(), "帧流头不足 32 字节");
        RejectPartial(path, metadata, CreateStream(3, CreateFrame(3, 0, 0)), "V3 首帧缺编队快照");
        RejectPartial(path, metadata, CreateStream(3, CreateFrame(3, 0, 4)), "V3 首帧保留标志位");
        RejectPartial(path, metadata, CreateStream(2, CreateFrame(2, 1, 0)), "V2 首帧号不是零");
        RejectPartial(path, metadata, CreateStream(2, CreateFrame(2, 0, 2)), "V2 首帧使用组标志");
        RejectPartial(path, metadata, CreateStream(3, Change(CreateFrame(3, 0, 2), 8, 16385)), "V3 首帧事件数量超限");

        // 帧流头无效：魔数、版本、事件大小、起始帧。
        RejectPartial(path, metadata, Change(valid, 0, 0), "未完成录像错误魔数");
        // 版本 4、5 已是有效版本（见 RunGameSpeed），未知版本改用 6 与最大值。
        RejectPartial(path, metadata, Change(valid, 4, 6), "未完成录像未知版本");
        Recover(path, metadata, Change(valid, 4, 5), 5, 5, valid.Length, "V3 联机帧流改为版本 5 按 V4 规则完整恢复");
        RejectPartial(path, metadata, Change(valid, 4, uint.MaxValue), "未完成录像版本为最大值");
        RejectPartial(path, metadata, Change(valid, 4, 0), "未完成录像版本为零");
        RejectPartial(path, metadata, Change(valid, 8, 110), "未完成录像流头事件尺寸");
        RejectPartial(path, metadata, Change(valid, 20, 1), "未完成录像流头非零起始帧");

        // 容器头：flags==0 必须 streamBytes==0 且内容哈希全零；flags 只能是 0 或 1。
        byte[] zeroHash = new byte[32];
        byte[] realHash = Hash(metadata.Concat(valid).ToArray());
        WriteContainer(path, metadata, valid, 0, (ulong)valid.Length, zeroHash, (uint)metadata.Length);
        RejectOpen(path, "flags=0 但 streamBytes 为帧流长度");
        WriteContainer(path, metadata, valid, 0, 1, zeroHash, (uint)metadata.Length);
        RejectOpen(path, "flags=0 但 streamBytes 为 1");
        WriteContainer(path, metadata, valid, 0, ulong.MaxValue, zeroHash, (uint)metadata.Length);
        RejectOpen(path, "flags=0 但 streamBytes 为最大值");
        WriteContainer(path, metadata, valid, 0, 0, realHash, (uint)metadata.Length);
        RejectOpen(path, "flags=0 但内容哈希为实际哈希");
        byte[] firstByte = new byte[32];
        firstByte[0] = 1;
        WriteContainer(path, metadata, valid, 0, 0, firstByte, (uint)metadata.Length);
        RejectOpen(path, "flags=0 但内容哈希首字节非零");
        byte[] lastByte = new byte[32];
        lastByte[31] = 0x80;
        WriteContainer(path, metadata, valid, 0, 0, lastByte, (uint)metadata.Length);
        RejectOpen(path, "flags=0 但内容哈希末字节非零");
        foreach (uint flags in new uint[] { 2, 3, 0x80000000u, uint.MaxValue })
        {
            WriteContainer(path, metadata, valid, flags, (ulong)valid.Length, realHash, (uint)metadata.Length);
            RejectOpen(path, "flags=" + flags + " 带完成文件字段");
            WriteContainer(path, metadata, valid, flags, 0, zeroHash, (uint)metadata.Length);
            RejectOpen(path, "flags=" + flags + " 带未完成字段");
        }
        WriteContainer(path, metadata, valid, 1, 0, zeroHash, (uint)metadata.Length);
        RejectOpen(path, "flags=1 但 streamBytes 为零");
        WriteContainer(path, metadata, valid, 1, (ulong)valid.Length, zeroHash, (uint)metadata.Length);
        ReplayArchive hashless = ReplayArchive.Open(path);
        bool hashRejected = false;
        try { hashless.ValidatePayload(); }
        catch (InvalidDataException) { hashRejected = true; }
        Check(!hashless.IsUnfinalized && hashRejected, "flags=1 的完成文件仍严格校验内容哈希");
        WriteContainer(path, metadata, valid, 0, 0, zeroHash, 0);
        RejectOpen(path, "metadataBytes 为零");
        WriteContainer(path, metadata, valid, 0, 0, zeroHash, 16u * 1024 * 1024 + 1);
        RejectOpen(path, "metadataBytes 超过上限");
        WriteContainer(path, metadata, valid, 0, 0, zeroHash, (uint)metadata.Length + 1000);
        RejectOpen(path, "metadataBytes 超出文件");
        WriteContainer(path, metadata, valid, 0, 0, zeroHash, (uint)metadata.Length + 1);
        RejectOpen(path, "metadataBytes 吞掉帧流头");
        WritePartial(path, new byte[100], valid);
        RejectOpen(path, "metadata 清单无法解析");
        WritePartial(path, metadata, valid);
        byte[] shortFile = File.ReadAllBytes(path);
        File.WriteAllBytes(path, shortFile.Take(96 + metadata.Length + 31).ToArray());
        RejectOpen(path, "容器内帧流头不足 32 字节");
        File.WriteAllBytes(path, shortFile.Take(96 + metadata.Length).ToArray());
        RejectOpen(path, "容器没有帧流");
        File.WriteAllBytes(path, shortFile.Take(96 + metadata.Length - 1).ToArray());
        RejectOpen(path, "容器 metadata 被截断");

        // 1 GiB 上限：候选帧流长度（文件长度 - 96 - metadataBytes）恰为 1 GiB 时可恢复；
        // 多 1 字节即直接拒绝，不截断到 1 GiB 后再扫描。NTFS 上 SetLength 只扩展长度，扩出部分读作零。
        long gibibyte = 1024L * 1024 * 1024;
        WritePartial(path, metadata, valid);
        using (FileStream large = new FileStream(path, FileMode.Open, FileAccess.Write, FileShare.None))
            large.SetLength(96 + metadata.Length + gibibyte);
        ReplayArchive atLimit = ReplayArchive.Open(path);
        atLimit.ValidatePayload();
        Check(atLimit.IsUnfinalized && atLimit.RecoveredFrameCount == 5 && atLimit.StreamBytes == valid.Length &&
            atLimit.MatchesCurrentHeader(), "1 GiB 候选帧流在零尾部之前恢复");
        using (FileStream large = new FileStream(path, FileMode.Open, FileAccess.Write, FileShare.None))
            large.SetLength(96 + metadata.Length + gibibyte + 1);
        RejectOpen(path, "候选帧流超过 1 GiB 直接拒绝");
        WritePartial(path, metadata, valid);
        using (FileStream large = new FileStream(path, FileMode.Open, FileAccess.Write, FileShare.None))
            large.SetLength(96 + metadata.Length + gibibyte + 4096);
        RejectOpen(path, "候选帧流超过 1 GiB 即使有效帧位于开头");
        File.Delete(path);

        if (!string.IsNullOrEmpty(fixtureDirectory))
        {
            byte[] cppV2 = File.ReadAllBytes(Path.Combine(fixtureDirectory, "rpf2-groups-v2.bin"));
            byte[] cppV3 = File.ReadAllBytes(Path.Combine(fixtureDirectory, "rpf2-groups-v3.bin"));
            // 起始偏移与长度：V2 帧 0 为 32..52、帧 1 为 52..223。
            Recover(path, metadata, cppV2, 2, 2, 223, "实际 C++ V2 作为未完成录像完整恢复");
            CheckPrefixes(path, metadata, cppV2, 2, new int[] { 52, 223 }, "实际 C++ V2 未完成");
            Recover(path, metadata, cppV3, 3, 5, 363, "实际 C++ V3 作为未完成录像完整恢复");
            CheckPrefixes(path, metadata, cppV3, 3, new int[] { 56, 267, 315, 339, 363 }, "实际 C++ V3 未完成");
            Recover(path, metadata, Change(cppV3, 267 + 4, 7), 3, 5, 363, "实际 C++ V3 帧校验和取值不影响恢复");
            Recover(path, metadata, Change(cppV3, 267, 9), 3, 2, 267, "实际 C++ V3 帧 2 帧号错误处截止");
            Console.WriteLine("实际 C++ RPF2 样本作为未完成录像恢复通过");
        }
    }

    // 版本 4 起：flags bit2 表示帧头后带 4 字节游戏速度（0..6）。单机（gameMode 0/5）帧 0 必须带速度，
    // 后续帧可带可不带；联机任何帧都不得带速度；版本 1–3 不允许 bit2。完成文件与未完成录像按同一套规则。
    // 版本 5 的布局与规则与版本 4 完全相同（只表示单机录制时已排除飞机绘制对模拟的影响），以同一组用例分别运行；
    // 与版本无关的版本 1–3 用例只在版本 4 一轮运行。
    private static void RunGameSpeed(string root, byte[] metadata, uint version)
    {
        string v = "V" + version;
        string archivePath = Path.Combine(root, "game-speed.yrrep");
        string partialPath = Path.Combine(root, "game-speed.yrrep.partial");
        // 单机：帧 0 速度 6 与空快照；帧 1 无速度带事件；帧 2 改速 3 并带事件与效果；帧 3 快照与速度 2；帧 4 无速度带效果。
        byte[] s0 = CreateV4Frame(0, 2 | 4, 6, 0, 0);
        byte[] s1 = CreateV4Frame(1, 0, 0, 1, 0);
        byte[] s2 = CreateV4Frame(2, 4, 3, 2, 1);
        byte[] s3 = CreateV4Frame(3, 1 | 2 | 4, 2, 0, 0, 0, 101, 52, 9, 202, 52);
        byte[] s4 = CreateV4Frame(4, 1, 0, 0, 2);
        byte[] skirmish = CreateModeStream(version, 5, s0, s1, s2, s3, s4);
        int[] skirmishEnds = Boundaries(s0, s1, s2, s3, s4);
        Check(s0.Length == 28 && s1.Length == 24 + 111 && s2.Length == 28 + 222 + 40 && s3.Length == 28 + 24 &&
            s4.Length == 24 + 80, v + " 帧记录长度：带速度多 4 字节");
        Accept(archivePath, metadata, skirmish, version, v + " 遭遇战帧 0 带速度，其后有无速度均可");
        Accept(archivePath, metadata, CreateModeStream(version, 0, s0, s1, s2, s3, s4), version, v + " 战役按单机规则");
        Accept(archivePath, metadata, CreateModeStream(version, 5, s0), version, v + " 单机只有帧 0");
        Accept(archivePath, metadata, CreateModeStream(version, 5, CreateV4Frame(0, 2 | 4, 0, 0, 0), CreateV4Frame(1, 4, 6, 0, 0)),
            version, v + " 速度上下界 0 与 6");
        Check(ReplayArchive.Open(archivePath).CountFrames() == 2u, v + " 完成文件帧数统计");
        Accept(archivePath, metadata, CreateModeStream(version, 5, s0, CreateV4Frame(1, 0, 0, 0, 0), CreateV4Frame(2, 0, 0, 0, 0)),
            version, v + " 单机非 0 帧没有速度");
        WriteArchive(archivePath, metadata, skirmish);
        Check(ReplayArchive.Open(archivePath).CountFrames() == 5u, v + " 带速度帧的帧数统计");
        // 联机：任何帧都不带速度，记录布局与 V3 相同。
        byte[] m0 = CreateV4Frame(0, 2, 0, 1, 1, 0, 101, 52);
        byte[] m1 = CreateV4Frame(1, 1, 0, 0, 0);
        byte[] m2 = CreateV4Frame(2, 3, 0, 2, 0, 1, 103, 52);
        int[] multiplayerEnds = Boundaries(m0, m1, m2);
        foreach (uint mode in new uint[] { 1, 2, 3, 4, 6 })
            Accept(archivePath, metadata, CreateModeStream(version, mode, m0, m1, m2), version, v + " 联机不带速度 gameMode=" + mode);

        // 完成文件的拒绝规则。
        Reject(archivePath, metadata, CreateModeStream(version, 5), v + " 单机零帧");
        Reject(archivePath, metadata, CreateModeStream(version, 3), v + " 联机零帧");
        Reject(archivePath, metadata, CreateModeStream(version, 5, CreateV4Frame(0, 2, 0, 0, 0)), v + " 遭遇战帧 0 缺速度");
        Reject(archivePath, metadata, CreateModeStream(version, 0, CreateV4Frame(0, 2, 0, 0, 0), s1), v + " 战役帧 0 缺速度");
        Reject(archivePath, metadata, CreateModeStream(version, 5, CreateV4Frame(0, 4, 6, 0, 0)), v + " 帧 0 有速度但缺编队快照");
        Reject(archivePath, metadata, CreateModeStream(version, 5, CreateV4Frame(0, 2 | 4, 7, 0, 0)), v + " 帧 0 速度 7");
        Reject(archivePath, metadata, CreateModeStream(version, 5, s0, s1, CreateV4Frame(2, 4, 7, 0, 0)), v + " 后续帧速度 7");
        Reject(archivePath, metadata, CreateModeStream(version, 5, CreateV4Frame(0, 2 | 4, uint.MaxValue, 0, 0)), v + " 速度为最大值");
        foreach (uint flag in new uint[] { 8, 16, 0x80000000u })
        {
            Reject(archivePath, metadata, CreateModeStream(version, 5, CreateV4Frame(0, 2 | 4 | flag, 6, 0, 0)),
                v + " 帧 0 未知标志位 " + flag);
            Reject(archivePath, metadata, CreateModeStream(version, 5, s0, CreateV4Frame(1, flag, 0, 0, 0)),
                v + " 后续帧未知标志位 " + flag);
            Reject(archivePath, metadata, CreateModeStream(version, 3, m0, CreateV4Frame(1, flag, 0, 0, 0)),
                v + " 联机后续帧未知标志位 " + flag);
        }
        foreach (uint mode in new uint[] { 1, 2, 3, 4, 6 })
        {
            Reject(archivePath, metadata, CreateModeStream(version, mode, CreateV4Frame(0, 2 | 4, 6, 0, 0)),
                v + " 联机帧 0 带速度 gameMode=" + mode);
            Reject(archivePath, metadata, CreateModeStream(version, mode, m0, CreateV4Frame(1, 4, 0, 0, 0)),
                v + " 联机后续帧带速度 gameMode=" + mode);
        }
        // 版本 4、5 之后的版本未知：完成文件拒绝。
        Reject(archivePath, metadata, Change(skirmish, 4, 6), v + " 单机帧流改为未知版本 6");
        Reject(archivePath, metadata, Change(skirmish, 4, uint.MaxValue), v + " 单机帧流改为版本最大值");
        if (version == 4)
        {
            // 版本 1–3 不允许 bit2：无论帧流头是单机还是联机、记录是否附带速度字段。
            Reject(archivePath, metadata, CreateModeStream(3, 5, CreateFrame(3, 0, 2 | 4)), "V3 单机帧 0 置 bit2");
            Reject(archivePath, metadata, CreateModeStream(3, 3, CreateFrame(3, 0, 2 | 4)), "V3 联机帧 0 置 bit2");
            Reject(archivePath, metadata, CreateModeStream(3, 5, CreateFrame(3, 0, 2), CreateFrame(3, 1, 4)), "V3 后续帧置 bit2");
            Reject(archivePath, metadata, CreateModeStream(3, 5, s0), "V3 帧流头配 V4 带速度记录");
            Reject(archivePath, metadata, CreateModeStream(2, 5, CreateFrame(2, 0, 4)), "V2 置 bit2");
            Reject(archivePath, metadata, CreateModeStream(1, 5, CreateFrame(1, 0, 4)), "V1 置 bit2");
            // 版本 1–3 在单机帧流头下行为不变：不要求速度。
            Accept(archivePath, metadata, CreateModeStream(3, 5, CreateFrame(3, 0, 2), CreateFrame(3, 1, 0)), 3, "V3 单机无速度");
            Accept(archivePath, metadata, CreateModeStream(2, 0, CreateFrame(2, 0, 1)), 2, "V2 战役无速度");
            Accept(archivePath, metadata, CreateModeStream(1, 5, CreateFrame(1, 0, 1)), 1, "V1 遭遇战无速度");
        }
        else
        {
            // 版本 5 与版本 4 的帧流只差帧流头的版本字段，记录逐字节相同。
            byte[] v4Skirmish = CreateModeStream(4, 5, s0, s1, s2, s3, s4);
            Check(v4Skirmish.Length == skirmish.Length && Change(v4Skirmish, 4, version).SequenceEqual(skirmish),
                v + " 与 V4 帧流只差帧流头版本字段");
        }
        // 仅在完整帧边界结束的前缀合法；逐字节截断覆盖速度字段、事件、效果与成员。
        for (int length = 32; length < skirmish.Length; length++)
            if (!skirmishEnds.Contains(length))
                Reject(archivePath, metadata, skirmish.Take(length).ToArray(), v + " 截断位置 " + length);
        File.Delete(archivePath);

        // 未完成录像：完整恢复、逐字节截断与第一条无效记录处截止。
        Recover(partialPath, metadata, skirmish, version, 5, skirmish.Length, v + " 单机完整恢复");
        CheckPrefixes(partialPath, metadata, skirmish, version, skirmishEnds, v + " 单机未完成");
        Recover(partialPath, metadata, skirmish.Take(skirmishEnds[1] + 24 + 2).ToArray(), version, 2, skirmishEnds[1],
            v + " 速度字段中途截断");
        Recover(partialPath, metadata, skirmish.Take(skirmishEnds[1] + 28 + 111 + 5).ToArray(), version, 2, skirmishEnds[1],
            v + " 带速度帧的事件中途截断");
        byte[] multiplayer = CreateModeStream(version, 3, m0, m1, m2);
        Recover(partialPath, metadata, multiplayer, version, 3, multiplayer.Length, v + " 联机完整恢复");
        CheckPrefixes(partialPath, metadata, multiplayer, version, multiplayerEnds, v + " 联机未完成");
        byte[] tail = CreateV4Frame(3, 0, 0, 0, 0);
        Recover(partialPath, metadata, CreateModeStream(version, 5, s0, s1, CreateV4Frame(2, 4, 7, 0, 0), tail), version, 2,
            skirmishEnds[1], v + " 中间帧速度 7 处截止");
        Recover(partialPath, metadata, CreateModeStream(version, 5, s0, s1, CreateV4Frame(2, 4, uint.MaxValue, 0, 0), tail), version, 2,
            skirmishEnds[1], v + " 中间帧速度最大值处截止");
        Recover(partialPath, metadata, CreateModeStream(version, 5, s0, s1, CreateV4Frame(2, 8, 0, 0, 0), tail), version, 2,
            skirmishEnds[1], v + " 中间帧未知标志位处截止");
        Recover(partialPath, metadata, CreateModeStream(version, 5, s0, s1, CreateV4Frame(2, 0x80000000u | 4, 1, 0, 0), tail), version, 2,
            skirmishEnds[1], v + " 中间帧最高标志位处截止");
        Recover(partialPath, metadata, CreateModeStream(version, 3, m0, m1, CreateV4Frame(2, 4, 1, 0, 0), tail), version, 2,
            multiplayerEnds[1], v + " 联机中间帧带速度处截止");
        Recover(partialPath, metadata, CreateModeStream(version, 5, s0, s1, CreateV4Frame(3, 4, 1, 0, 0), tail), version, 2,
            skirmishEnds[1], v + " 中间帧号跳跃处截止");
        Recover(partialPath, metadata, skirmish.Concat(new byte[64]).ToArray(), version, 5, skirmish.Length, v + " 全零尾部");
        if (version == 4)
            Recover(partialPath, metadata, CreateModeStream(3, 5, CreateFrame(3, 0, 2), CreateFrame(3, 1, 4), CreateFrame(3, 2, 0)),
                3, 1, 32 + 24, "V3 中间帧置 bit2 处截止");
        RejectPartial(partialPath, metadata, CreateModeStream(version, 5), v + " 帧流只有头");
        RejectPartial(partialPath, metadata, CreateModeStream(version, 5, CreateV4Frame(0, 2, 0, 0, 0), s1),
            v + " 单机帧 0 缺速度没有可恢复帧");
        RejectPartial(partialPath, metadata, CreateModeStream(version, 0, CreateV4Frame(0, 2, 0, 0, 0)),
            v + " 战役帧 0 缺速度没有可恢复帧");
        RejectPartial(partialPath, metadata, CreateModeStream(version, 3, CreateV4Frame(0, 2 | 4, 6, 0, 0), m1),
            v + " 联机帧 0 带速度没有可恢复帧");
        RejectPartial(partialPath, metadata, CreateModeStream(version, 5, CreateV4Frame(0, 2 | 4, 7, 0, 0)), v + " 帧 0 速度 7");
        RejectPartial(partialPath, metadata, CreateModeStream(version, 5, CreateV4Frame(0, 2 | 4 | 8, 6, 0, 0)), v + " 帧 0 未知标志位");
        RejectPartial(partialPath, metadata, CreateModeStream(version, 5, CreateV4Frame(0, 4, 6, 0, 0)), v + " 帧 0 缺编队快照");
        RejectPartial(partialPath, metadata, skirmish.Take(32 + 26).ToArray(), v + " 帧 0 速度字段截断");
        if (version == 4)
            RejectPartial(partialPath, metadata, CreateModeStream(3, 5, CreateFrame(3, 0, 2 | 4)), "V3 帧 0 置 bit2");
        RejectPartial(partialPath, metadata, Change(skirmish, 4, 6), v + " 未完成录像改为未知版本 6");
        RejectPartial(partialPath, metadata, Change(skirmish, 4, uint.MaxValue), v + " 未完成录像改为版本最大值");
        File.Delete(partialPath);
    }

    // 完成文件的 Open 只取帧流头的版本与 gameMode，不新增失败路径：eventBytes、起始帧等字段无效时 Open 仍成功，
    // 由 ValidatePayload 拒绝；帧流不足 32 字节或长度不符时，Open 仍以原有错误文本拒绝。
    private static void RunStreamHeaderOpen(string root, byte[] metadata)
    {
        string path = Path.Combine(root, "stream-header.yrrep");
        byte[] valid = CreateModeStream(5, 5, CreateV4Frame(0, 2 | 4, 6, 0, 0));
        Func<string> openError = () =>
        {
            try { ReplayArchive.Open(path); return null; }
            catch (InvalidDataException error) { return error.Message; }
        };
        foreach (int[] field in new[] { new[] { 8, 110 }, new[] { 20, 1 } })
        {
            WriteArchive(path, metadata, Change(valid, field[0], (uint)field[1]));
            ReplayArchive opened = ReplayArchive.Open(path);
            bool rejected = false;
            try { opened.ValidatePayload(); }
            catch (InvalidDataException) { rejected = true; }
            Check(opened.StreamVersion == 5 && opened.StreamGameMode == 5u && rejected,
                "完成文件帧流头偏移 " + field[0] + " 无效时 Open 照常、ValidatePayload 拒绝");
        }
        foreach (int length in new[] { 0, 4, 8, 12, 15, 16, 31 })
        {
            WriteArchive(path, metadata, valid.Take(length).ToArray());
            Check(openError() == "回放长度或完成标志无效。", "完成文件帧流只有 " + length + " 字节时 Open 错误文本不变");
        }
        WriteContainer(path, metadata, valid.Take(16).ToArray(), 1, 32, Hash(metadata.Concat(valid).ToArray()),
            (uint)metadata.Length);
        Check(openError() == "回放长度或完成标志无效。", "完成文件声明 32 字节帧流但实际被截断时 Open 错误文本不变");
        WriteArchive(path, metadata, Change(valid, 4, 6));
        Check(openError() == "不支持的回放帧流版本。", "完成文件帧流版本 6 的 Open 错误文本不变");
        WriteArchive(path, metadata, Change(valid, 0, 0));
        Check(openError() == "回放帧流头无效。", "完成文件帧流魔数错误的 Open 错误文本不变");
        foreach (int length in new[] { 0, 16, 31 })
        {
            WritePartial(path, metadata, valid.Take(length).ToArray());
            Check(openError() == "未完成录像缺少完整的帧流头。", "未完成录像帧流只有 " + length + " 字节时 Open 错误文本不变");
        }
        File.Delete(path);
    }

    private static uint U32(byte[] data, int offset)
    {
        return BitConverter.ToUInt32(data, offset);
    }

    private static string HexHash(byte[] data)
    {
        return BitConverter.ToString(Hash(data)).Replace("-", "").ToLowerInvariant();
    }

    // 插件写出的 v4/v5 样例（tests/fixtures）。
    // 样例 metadata 是 5 字节占位而非有效开局清单，客户端不能直接打开：先逐字段核对容器头与内容哈希，
    // 再以测试自带的解析核对各帧偏移、长度、bit2 与速度，最后把帧流换上有效清单交给 ReplayArchive 校验与恢复。
    // v5 样例的帧内容与 v4 样例相同、只有帧流头版本为 5：以 v4 样例逐字节对照（完成样例另有随之变化的内容哈希），
    // 其余核对与 v4 一轮相同。
    private static void RunGameSpeedFixtures(string root, byte[] metadata, string fixtureDirectory, uint version)
    {
        string label = "插件 v" + version;
        byte[] finished = File.ReadAllBytes(Path.Combine(fixtureDirectory, "replay-v" + version + "-speed.yrrep"));
        byte[] partial = File.ReadAllBytes(Path.Combine(fixtureDirectory, "replay-v" + version + "-speed-partial.yrrep"));
        if (version == 4)
        {
            Check(finished.Length == 583 && HexHash(finished) == "790f7150123f63d40ea78ff51ff5bafa321d17bd279c79aeabea4bdbb4862562",
                "插件 v4 完成样例长度与 SHA-256");
            Check(partial.Length == 509 && HexHash(partial) == "3b81229a7b2136ae56401a70bffa2b0c4cc96ac8bc5057d337e172781200a081",
                "插件 v4 未完成样例长度与 SHA-256");
        }
        else
        {
            // 101 + 4 起的 4 字节是帧流头版本；完成样例 64..95 是内容哈希（下面另行按内容重算核对）。
            byte[] v4Finished = File.ReadAllBytes(Path.Combine(fixtureDirectory, "replay-v4-speed.yrrep"));
            byte[] v4Partial = File.ReadAllBytes(Path.Combine(fixtureDirectory, "replay-v4-speed-partial.yrrep"));
            Check(finished.Length == 583 && finished.Length == v4Finished.Length &&
                Enumerable.Range(0, finished.Length).All(i => (i >= 64 && i < 96) || (i >= 105 && i < 109) ||
                    finished[i] == v4Finished[i]),
                label + " 完成样例除内容哈希与帧流头版本外与 v4 样例逐字节相同");
            Check(partial.Length == 509 && partial.Length == v4Partial.Length &&
                Enumerable.Range(0, partial.Length).All(i => (i >= 105 && i < 109) || partial[i] == v4Partial[i]),
                label + " 未完成样例除帧流头版本外与 v4 样例逐字节相同");
            Console.WriteLine(label + " 完成样例 SHA-256 " + HexHash(finished) + "，未完成样例 SHA-256 " + HexHash(partial));
        }
        byte[] fixtureMetadata = { 0x41, 0x00, 0xFE, 0x42, 0x43 };
        byte[] gameHash = Enumerable.Range(1, 32).Select(value => (byte)value).ToArray();
        byte[] stream = finished.Skip(101).ToArray();
        byte[] candidate = partial.Skip(101).ToArray();
        foreach (byte[] file in new[] { finished, partial })
            Check(Encoding.ASCII.GetString(file, 0, 8) == "RAREPL2\0" && U32(file, 8) == 2 && U32(file, 12) == 96 &&
                U32(file, 16) == 5 && file.Skip(32).Take(32).SequenceEqual(gameHash) &&
                file.Skip(96).Take(5).SequenceEqual(fixtureMetadata), label + " 样例容器头与 metadata");
        Check(U32(finished, 20) == 1 && BitConverter.ToUInt64(finished, 24) == 482 && stream.Length == 482 &&
            (version != 4 || BitConverter.ToString(finished, 64, 32).Replace("-", "").ToLowerInvariant() ==
                "215e5d78261de44fe435a4fb99cc1f3820a19b2c23be6d935947b605ab046ff8") &&
            Hash(fixtureMetadata.Concat(stream).ToArray()).SequenceEqual(finished.Skip(64).Take(32)),
            label + " 完成样例：flags=1、streamBytes 与内容哈希");
        Check(U32(partial, 20) == 0 && BitConverter.ToUInt64(partial, 24) == 0 && partial.Skip(64).Take(32).All(value => value == 0) &&
            candidate.Length == 335 + 73 && candidate.SequenceEqual(stream.Take(candidate.Length)),
            label + " 未完成样例：flags=0、零长度与零哈希，候选帧流是完成样例前 408 字节");
        Check(U32(stream, 0) == 0x32465052u && U32(stream, 4) == version && U32(stream, 8) == 111 && U32(stream, 12) == 5 &&
            U32(stream, 16) == 1 && U32(stream, 20) == 0 && U32(stream, 24) == 6 && U32(stream, 28) == 0x12345678u,
            label + " 样例帧流头");

        // 按 v4 布局逐帧解析：帧头 24 字节，bit2 时紧跟 4 字节速度，再依次是事件、效果与成员。
        int[] offsets = { 32, 84, 219, 283, 311, 335 };
        int[] lengths = { 52, 135, 64, 28, 24, 147 };
        uint[] flags = { 6, 0, 0, 4, 0, 2 };
        uint?[] speeds = { 2u, null, null, 4u, null, null };
        int offset = 32;
        for (int frame = 0; frame < offsets.Length; frame++)
        {
            uint frameFlags = U32(stream, offset + 12);
            bool hasSpeed = (frameFlags & 4u) != 0;
            uint? speed = hasSpeed ? U32(stream, offset + 24) : (uint?)null;
            int length = 24 + (hasSpeed ? 4 : 0) + (int)(U32(stream, offset + 8) * 111 + U32(stream, offset + 16) * 40 +
                U32(stream, offset + 20) * 12);
            Check(offset == offsets[frame] && U32(stream, offset) == (uint)frame &&
                frameFlags == flags[frame] && speed == speeds[frame] && length == lengths[frame],
                label + " 样例帧 " + frame + " 的偏移、长度、bit2 与速度");
            offset += length;
        }
        Check(offset == stream.Length, label + " 完成样例在帧 5 之后正常结束");
        Func<byte, uint, byte, byte[]> gameEvent = (first, frame, last) =>
        {
            byte[] value = new byte[111];
            value[0] = first;
            Array.Copy(BitConverter.GetBytes(frame), 0, value, 3, 4);
            value[110] = last;
            return value;
        };
        Check(Enumerable.Range(0, 6).Select(i => U32(stream, 32 + 28 + i * 4)).SequenceEqual(new uint[] { 0, 101, 52, 9, 202, 52 }),
            label + " 样例帧 0 编队快照");
        Check(stream.Skip(84 + 24).Take(111).SequenceEqual(gameEvent(0xA1, 1, 0xB2)), label + " 样例帧 1 事件");
        Check(stream.Skip(219 + 24).Take(40).SequenceEqual(CreateEffect(1, 1)), label + " 样例帧 2 输入效果");
        Check(stream.Skip(335 + 24).Take(111).SequenceEqual(gameEvent(0xC3, 5, 0xD4)) &&
            Enumerable.Range(0, 3).Select(i => U32(stream, 335 + 24 + 111 + i * 4)).SequenceEqual(new uint[] { 1, 101, 52 }),
            label + " 样例帧 5 事件与编队快照");

        // 客户端校验：完成样例严格逐帧校验通过、帧数 6；未完成样例恢复帧 0–4，可恢复帧流到帧 5 之前（335 字节）。
        string path = Path.Combine(root, "fixture-v" + version + "-speed.yrrep");
        string partialPath = Path.Combine(root, "fixture-v" + version + "-speed.yrrep.partial");
        Accept(path, metadata, stream, version, label + " 完成样例换上有效清单后通过校验");
        Check(ReplayArchive.Open(path).CountFrames() == 6u, label + " 完成样例帧数 6");
        Recover(partialPath, metadata, candidate, version, 5, 335, label + " 未完成样例恢复 5 帧");
        Check(ReplayArchive.Open(partialPath).CountFrames() == 5u, label + " 未完成样例帧数为可恢复帧数");
        CheckPrefixes(partialPath, metadata, stream, version, offsets.Skip(1).Concat(new[] { stream.Length }).ToArray(),
            label + " 样例逐字节截断");
        // 改动样例的速度或帧流头：客户端按同一套规则拒绝或截止。
        Reject(path, metadata, Change(stream, 32 + 24, 7), label + " 样例帧 0 速度改为 7");
        Reject(path, metadata, Change(stream, 283 + 24, 7), label + " 样例帧 3 速度改为 7");
        Reject(path, metadata, Change(stream, 32 + 12, 2), label + " 样例帧 0 去掉 bit2");
        Reject(path, metadata, Change(stream, 12, 3), label + " 样例改为联机 gameMode");
        Reject(path, metadata, Change(stream, 4, 3), label + " 样例改为版本 3");
        Accept(path, metadata, Change(stream, 12, 0), version, label + " 样例改为战役 gameMode");
        Recover(partialPath, metadata, Change(candidate, 283 + 24, 7), version, 3, 283, label + " 未完成样例帧 3 速度 7 处截止");
        Recover(partialPath, metadata, Change(candidate, 283 + 12, 12), version, 3, 283, label + " 未完成样例帧 3 未知标志位处截止");
        RejectPartial(partialPath, metadata, Change(candidate, 12, 1), label + " 未完成样例改为联机 gameMode");
        if (version >= 5)
        {
            // 同一帧流把版本改回 4 仍按同一套规则通过；改为未知版本 6 则拒绝。
            Accept(path, metadata, Change(stream, 4, 4), 4, label + " 样例改为版本 4 按同一规则通过");
            Reject(path, metadata, Change(stream, 4, 6), label + " 样例改为版本 6");
            RejectPartial(partialPath, metadata, Change(candidate, 4, 6), label + " 未完成样例改为版本 6");
        }
        File.Delete(path);
        File.Delete(partialPath);
        Console.WriteLine(label + " 游戏速度样例核对通过");
    }

    // ===== 容器 v3：开局清单与帧流按独立块压缩 =====

    private const uint BlockMagic = 0x4B4C4252;
    private const uint FrameBlockLimit = 4u * 1024 * 1024;
    private const uint ManifestBlockLimit = 16u * 1024 * 1024;

    // 按 RFC 1951 写比特：数据字段先写低位，Huffman 码先写高位。用于手工构造 deflate 载荷。
    private sealed class DeflateBits
    {
        private readonly List<byte> bytes = new List<byte>();
        private int bitCount;

        public int BitCount { get { return bitCount; } }

        public void Bits(uint value, int count)
        {
            for (int i = 0; i < count; i++)
            {
                if (bitCount % 8 == 0) bytes.Add(0);
                if (((value >> i) & 1) != 0) bytes[bytes.Count - 1] |= (byte)(1 << (bitCount % 8));
                bitCount++;
            }
        }

        public void Code(uint code, int length)
        {
            for (int i = length - 1; i >= 0; i--) Bits((code >> i) & 1, 1);
        }

        public byte[] ToArray() { return bytes.ToArray(); }
    }

    // 固定 Huffman 块的块头（BFINAL、BTYPE=01）、字面量（0..143 为 8 位码，144..255 为 9 位码）、
    // 长度 258 距离 1 的匹配（符号 285 为 8 位码，距离码 0 为 5 位码，均无附加位）与块结束符号 256（7 位全零）。
    private static void FixedHeader(DeflateBits bits, bool final)
    {
        bits.Bits(final ? 1u : 0u, 1);
        bits.Bits(1, 2);
    }

    private static void FixedLiteral(DeflateBits bits, byte value)
    {
        if (value < 144) bits.Code(0x30u + value, 8);
        else bits.Code(0x190u + (uint)(value - 144), 9);
    }

    private static void FixedMatch258(DeflateBits bits)
    {
        bits.Code(0xC5, 8);
        bits.Code(0, 5);
    }

    private static void FixedEndOfBlock(DeflateBits bits)
    {
        bits.Code(0, 7);
    }

    // 逐位计算的 CRC-32 参照实现，用于核对表驱动实现。
    private static uint ReferenceCrc32(byte[] data)
    {
        uint crc = 0xFFFFFFFFu;
        foreach (byte value in data)
        {
            crc ^= value;
            for (int k = 0; k < 8; k++) crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
        return ~crc;
    }

    private static uint Crc(byte[] data)
    {
        return ReplayBlocks.Crc32(data, 0, data.Length);
    }

    private static byte[] Words(params uint[] values)
    {
        byte[] bytes = new byte[values.Length * 4];
        for (int i = 0; i < values.Length; i++) Array.Copy(BitConverter.GetBytes(values[i]), 0, bytes, i * 4, 4);
        return bytes;
    }

    // 任意字段的块：20 字节块头（magic、method、rawBytes、storedBytes、crc32）后接载荷。
    private static byte[] RawBlock(uint method, uint rawBytes, uint storedBytes, uint crc, byte[] payload,
        uint magic = BlockMagic)
    {
        return Words(magic, method, rawBytes, storedBytes, crc).Concat(payload).ToArray();
    }

    // 原始 deflate（.NET Framework 的压缩端使用 zlib）。
    private static byte[] DeflateBytes(byte[] raw)
    {
        using (MemoryStream output = new MemoryStream())
        {
            using (DeflateStream deflate = new DeflateStream(output, CompressionLevel.Optimal, true))
                deflate.Write(raw, 0, raw.Length);
            return output.ToArray();
        }
    }

    private static byte[] StoredBlock(byte[] raw)
    {
        return RawBlock(0, (uint)raw.Length, (uint)raw.Length, Crc(raw), raw);
    }

    private static byte[] DeflateBlock(byte[] raw)
    {
        byte[] packed = DeflateBytes(raw);
        Check(packed.Length < raw.Length, "测试数据可以压缩");
        return RawBlock(1, (uint)raw.Length, (uint)packed.Length, Crc(raw), packed);
    }

    // 写入端的选择：压缩结果小于原长时用 method 1，否则 method 0 原样存放。
    private static byte[] AutoBlock(byte[] raw)
    {
        byte[] packed = DeflateBytes(raw);
        return packed.Length < raw.Length ? RawBlock(1, (uint)raw.Length, (uint)packed.Length, Crc(raw), packed) : StoredBlock(raw);
    }

    // 在逻辑偏移 cuts（递增）处把逻辑帧流切成块，按写入端规则编码后首尾相接。
    private static byte[] Segment(byte[] logical, params int[] cuts)
    {
        List<byte> segment = new List<byte>();
        int start = 0;
        foreach (int cut in cuts.Concat(new[] { logical.Length }))
        {
            if (cut <= start) continue;
            segment.AddRange(AutoBlock(logical.Skip(start).Take(cut - start).ToArray()));
            start = cut;
        }
        return segment.ToArray();
    }

    // 帧流段内各块结束处的磁盘偏移（按块头 storedBytes 前进）。
    private static int[] BlockEnds(byte[] segment)
    {
        List<int> ends = new List<int>();
        int offset = 0;
        while (offset < segment.Length)
        {
            offset += 20 + (int)U32(segment, offset + 12);
            ends.Add(offset);
        }
        return ends.ToArray();
    }

    private static void WriteV3(string path, byte[] manifestSegment, byte[] frameSegment, uint flags, ulong streamBytes,
        byte[] contentHash, uint metadataBytes, uint version = 3)
    {
        using (FileStream output = File.Create(path))
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(Encoding.ASCII.GetBytes("RAREPL2\0"));
            writer.Write(version);
            writer.Write(96u);
            writer.Write(metadataBytes);
            writer.Write(flags);
            writer.Write(streamBytes);
            writer.Write(Enumerable.Repeat((byte)0x11, 32).ToArray());
            writer.Write(contentHash);
            writer.Write(manifestSegment);
            writer.Write(frameSegment);
        }
    }

    // 完成文件：清单块加帧流段，streamBytes 与内容哈希按实际磁盘字节填写。
    private static void WriteV3Finished(string path, byte[] metadata, byte[] frameSegment)
    {
        byte[] manifest = AutoBlock(metadata);
        WriteV3(path, manifest, frameSegment, 1, (ulong)frameSegment.Length,
            Hash(manifest.Concat(frameSegment).ToArray()), (uint)manifest.Length);
    }

    // 未完成录像：flags=0、streamBytes=0、内容哈希全零，清单块之后是候选帧流段。
    private static void WriteV3Partial(string path, byte[] metadata, byte[] candidate)
    {
        byte[] manifest = AutoBlock(metadata);
        WriteV3(path, manifest, candidate, 0, 0, new byte[32], (uint)manifest.Length);
    }

    private static bool Fails(Action action)
    {
        try { action(); return false; }
        catch (InvalidDataException) { return true; }
        catch (EndOfStreamException) { return true; }
    }

    private static string OpenError(string path)
    {
        try { ReplayArchive.Open(path); return null; }
        catch (InvalidDataException error) { return error.Message; }
    }

    // v3 完成文件通过 Open、ValidatePayload、帧数统计与头部比对；StreamBytes 为帧流段磁盘字节数。
    private static void AcceptV3(string path, byte[] metadata, byte[] segment, byte[] logical, uint version,
        uint frames, string name)
    {
        WriteV3Finished(path, metadata, segment);
        ReplayArchive archive = ReplayArchive.Open(path);
        archive.ValidatePayload();
        Check(archive.ContainerVersion == 3 && !archive.IsUnfinalized && archive.RecoveredFrameCount == 0 &&
            archive.StreamBytes == segment.Length && archive.StreamVersion == version &&
            archive.StreamGameMode == U32(logical, 12) && archive.CountFrames() == frames &&
            archive.MatchesCurrentHeader(), name);
    }

    // v3 完成文件在帧流头之后才出错：Open 照常，ValidatePayload 拒绝，帧数统计不给出数字。
    private static void RejectV3Later(string path, byte[] metadata, byte[] segment, string name)
    {
        WriteV3Finished(path, metadata, segment);
        ReplayArchive archive = ReplayArchive.Open(path);
        Check(Fails(archive.ValidatePayload) && archive.CountFrames() == null, name);
    }

    // v3 未完成录像恢复为 frames 帧，StreamBytes 为有效块前缀的磁盘字节数。
    private static void RecoverV3(string path, byte[] metadata, byte[] candidate, uint version, uint frames,
        long validDiskBytes, string name)
    {
        WriteV3Partial(path, metadata, candidate);
        ReplayArchive archive = ReplayArchive.Open(path);
        archive.ValidatePayload();
        Check(archive.ContainerVersion == 3 && archive.IsUnfinalized && archive.StreamVersion == version &&
            archive.RecoveredFrameCount == frames && archive.StreamBytes == validDiskBytes &&
            archive.CountFrames() == frames && archive.MatchesCurrentHeader(), name);
    }

    private static void RejectV3Partial(string path, byte[] metadata, byte[] candidate, string name)
    {
        WriteV3Partial(path, metadata, candidate);
        RejectOpen(path, name);
    }

    private static void AcceptBlock(byte[] block, byte[] expected, uint maxRawBytes, string name)
    {
        ReplayBlockHeader header;
        byte[] raw;
        Check(ReplayBlocks.TryDecodeBlock(block, 0, block.Length, maxRawBytes, out header, out raw) &&
            raw.SequenceEqual(expected) && header.DiskBytes == block.Length, name);
    }

    private static void RejectBlock(byte[] block, int available, uint maxRawBytes, string name)
    {
        ReplayBlockHeader header;
        byte[] raw;
        Check(!ReplayBlocks.TryDecodeBlock(block, 0, available, maxRawBytes, out header, out raw) && raw == null, name);
    }

    // 每个元素只违反块有效性规则中的一条（以可压缩的 raw 为基础）；载荷长度与块头 storedBytes 一致，不影响后续块的位置。
    private static List<KeyValuePair<string, byte[]>> InvalidBlocks(byte[] raw)
    {
        byte[] packed = DeflateBytes(raw);
        Check(packed.Length < raw.Length - 1, "无效块用例的基础数据可以压缩");
        uint n = (uint)raw.Length;
        uint p = (uint)packed.Length;
        uint crc = Crc(raw);
        List<KeyValuePair<string, byte[]>> blocks = new List<KeyValuePair<string, byte[]>>();
        Action<string, byte[]> add = (name, block) => blocks.Add(new KeyValuePair<string, byte[]>(name, block));
        add("magic 错误", RawBlock(1, n, p, crc, packed, 0x4B4C4253));
        add("method 为 2", RawBlock(2, n, p, crc, packed));
        add("method 为最大值", RawBlock(uint.MaxValue, n, p, crc, packed));
        add("rawBytes 为 0", RawBlock(0, 0, 0, 0, new byte[0]));
        add("rawBytes 超过 4 MiB", RawBlock(1, FrameBlockLimit + 1, p, crc, packed));
        add("method 0 的 storedBytes 小于 rawBytes", RawBlock(0, n, n - 1, crc, raw.Take((int)n - 1).ToArray()));
        add("method 0 的 storedBytes 大于 rawBytes", RawBlock(0, n, n + 1, crc, raw.Concat(new byte[1]).ToArray()));
        add("method 1 的 storedBytes 为 0", RawBlock(1, n, 0, crc, new byte[0]));
        add("method 1 的 storedBytes 等于 rawBytes", RawBlock(1, n, n, crc, packed.Concat(new byte[n - p]).ToArray()));
        add("method 1 的 storedBytes 大于 rawBytes", RawBlock(1, n, n + 1, crc, packed.Concat(new byte[n + 1 - p]).ToArray()));
        add("method 0 的 CRC 不符", RawBlock(0, n, n, crc ^ 1, raw));
        add("method 1 的 CRC 不符", RawBlock(1, n, p, crc ^ 0x80000000u, packed));
        add("deflate 块类型 3", RawBlock(1, n, 3, crc, new byte[] { 0x07, 0x00, 0x00 }));
        add("deflate 存储块长度与补码不符", RawBlock(1, n, 7, crc, new byte[] { 0x01, 0x02, 0x00, 0x00, 0x00, 0x41, 0x42 }));
        add("解码少于 rawBytes", RawBlock(1, n + 1, p, Crc(raw.Concat(new byte[1]).ToArray()), packed));
        add("解码多于 rawBytes", RawBlock(1, n - 1, p, Crc(raw.Take((int)n - 1).ToArray()), packed));
        return blocks;
    }

    // 块解析、解码、CRC-32 与只前进的逻辑流。
    // 两侧解码器的已知残余差异：“距离超出已输出长度”“动态块码集不完整”“固定块距离码 30/31”
    // 这三类载荷 zlib 报错而 .NET Framework 的托管解码器不报错，两侧结论可能不同，这里不写断言两侧一致的用例。
    private static void RunBlockCodec()
    {
        Check(ReplayBlocks.Crc32(Encoding.ASCII.GetBytes("123456789"), 0, 9) == 0xCBF43926u, "CRC-32 测试向量 123456789");
        Check(ReplayBlocks.Crc32(new byte[0], 0, 0) == 0, "CRC-32 空输入");
        Random random = new Random(20261028);
        for (int length = 1; length <= 1200; length += 97)
        {
            byte[] data = new byte[length];
            random.NextBytes(data);
            Check(ReplayBlocks.Crc32(data, 0, length) == ReferenceCrc32(data) &&
                ReplayBlocks.Crc32(new byte[7].Concat(data).Concat(new byte[5]).ToArray(), 7, length) == ReferenceCrc32(data),
                "CRC-32 与逐位参照实现一致，长度 " + length);
        }

        byte[] compressible = Enumerable.Range(0, 5000).Select(i => (byte)(i % 9 == 0 ? i % 251 : 0x41)).ToArray();
        byte[] incompressible = new byte[3000];
        random.NextBytes(incompressible);
        AcceptBlock(DeflateBlock(compressible), compressible, FrameBlockLimit, "method 1 往返");
        AcceptBlock(StoredBlock(incompressible), incompressible, FrameBlockLimit, "method 0 往返");
        AcceptBlock(StoredBlock(new byte[] { 0x5A }), new byte[] { 0x5A }, FrameBlockLimit, "method 0 单字节");
        Check(U32(AutoBlock(compressible), 4) == 1 && U32(AutoBlock(incompressible), 4) == 0 && U32(AutoBlock(new byte[5]), 4) == 0,
            "写入端规则：可压缩数据用 method 1，不可压缩或过短的数据用 method 0");
        byte[] followed = StoredBlock(incompressible).Concat(new byte[9]).ToArray();
        ReplayBlockHeader header;
        byte[] decoded;
        Check(ReplayBlocks.TryDecodeBlock(followed, 0, followed.Length, FrameBlockLimit, out header, out decoded) &&
            decoded.SequenceEqual(incompressible) && header.DiskBytes == followed.Length - 9, "块之后的字节不属于本块");

        // 块头上限：帧流块 rawBytes 1..4 MiB，清单块 1..16 MiB。
        Check(ReplayBlocks.TryParseHeader(Words(BlockMagic, 0, FrameBlockLimit, FrameBlockLimit, 0), 0, FrameBlockLimit, out header) &&
            !ReplayBlocks.TryParseHeader(Words(BlockMagic, 0, FrameBlockLimit + 1, FrameBlockLimit + 1, 0), 0, FrameBlockLimit, out header) &&
            ReplayBlocks.TryParseHeader(Words(BlockMagic, 1, FrameBlockLimit, 1, 0), 0, FrameBlockLimit, out header) &&
            !ReplayBlocks.TryParseHeader(Words(BlockMagic, 1, FrameBlockLimit + 1, 1, 0), 0, FrameBlockLimit, out header),
            "帧流块 rawBytes 上限 4 MiB");
        Check(ReplayBlocks.TryParseHeader(Words(BlockMagic, 0, ManifestBlockLimit, ManifestBlockLimit, 0), 0, ManifestBlockLimit, out header) &&
            !ReplayBlocks.TryParseHeader(Words(BlockMagic, 0, ManifestBlockLimit + 1, ManifestBlockLimit + 1, 0), 0, ManifestBlockLimit, out header) &&
            ReplayBlocks.TryParseHeader(Words(BlockMagic, 1, 2, 1, 7), 0, ManifestBlockLimit, out header) &&
            header.Method == 1 && header.RawBytes == 2 && header.StoredBytes == 1 && header.Crc32 == 7 && header.DiskBytes == 21,
            "清单块 rawBytes 上限 16 MiB，块头字段按小端读取");

        // 块有效性规则中的每条无效条件。
        foreach (KeyValuePair<string, byte[]> invalid in InvalidBlocks(compressible))
            RejectBlock(invalid.Value, invalid.Value.Length, FrameBlockLimit, "无效块：" + invalid.Key);
        byte[] storedBlock = StoredBlock(incompressible);
        byte[] deflateBlock = DeflateBlock(compressible);
        RejectBlock(storedBlock, storedBlock.Length - 1, FrameBlockLimit, "无效块：method 0 载荷缺 1 字节");
        RejectBlock(deflateBlock, deflateBlock.Length - 1, FrameBlockLimit, "无效块：method 1 载荷缺 1 字节");
        RejectBlock(deflateBlock, 19, FrameBlockLimit, "无效块：块头不足 20 字节");
        RejectBlock(deflateBlock, 0, FrameBlockLimit, "无效块：没有任何字节");
        byte[] changedPayload = (byte[])storedBlock.Clone();
        changedPayload[100] ^= 0x10;
        RejectBlock(changedPayload, changedPayload.Length, FrameBlockLimit, "无效块：method 0 载荷被改动");

        // 不要求最终块标记，deflate 流结束之后载荷内的剩余字节不检查。
        byte[] packed = DeflateBytes(compressible);
        byte[] trailing = packed.Concat(new byte[] { 0xFF, 0x00, 0x13 }).ToArray();
        AcceptBlock(RawBlock(1, (uint)compressible.Length, (uint)trailing.Length, Crc(compressible), trailing), compressible,
            FrameBlockLimit, "deflate 流结束之后的剩余载荷不检查");
        DeflateBits notFinal = new DeflateBits();
        FixedHeader(notFinal, false);
        FixedLiteral(notFinal, 0x41);
        for (int i = 0; i < 8; i++) FixedMatch258(notFinal);
        FixedEndOfBlock(notFinal);
        byte[] repeated = Enumerable.Repeat((byte)0x41, 1 + 8 * 258).ToArray();
        byte[] notFinalPayload = notFinal.ToArray();
        AcceptBlock(RawBlock(1, (uint)repeated.Length, (uint)notFinalPayload.Length, Crc(repeated), notFinalPayload), repeated,
            FrameBlockLimit, "没有最终块标记的 deflate 流在输入耗尽处结束");

        // 块中途输入耗尽：5 个 9 位字面量与 8 个匹配正好占满 19 字节（3 + 45 + 104 = 152 位），块结束符号在第 20 字节。
        // 载荷只取前 19 字节时，按 zlib 规则解出恰好 rawBytes 字节、之后再无输出，块有效；
        // .NET Framework 的托管解码器此时第一次 Read 先返回 0、下一次才返回已解出的字节，C# 也必须判为有效。
        // 目标框架 4.7.2 及以上的进程（本测试程序与 DTAClient.exe 都是 4.8）默认改用 zlib 解压，第一次 Read 就返回全部字节；
        // 未声明目标框架的程序（如直接用 csc 编译的测试）仍走托管解码器。两种情形下块都必须判为有效。
        DeflateBits cut = new DeflateBits();
        FixedHeader(cut, true);
        for (int i = 0; i < 5; i++) FixedLiteral(cut, 200);
        for (int i = 0; i < 8; i++) FixedMatch258(cut);
        Check(cut.BitCount == 152, "块结束符号恰好从字节边界开始");
        FixedEndOfBlock(cut);
        byte[] full = cut.ToArray();
        byte[] cutRaw = Enumerable.Repeat((byte)200, 5 + 8 * 258).ToArray();
        byte[] withoutEnd = full.Take(19).ToArray();
        Check(full.Length == 20, "完整的手工 deflate 载荷为 20 字节");
        int firstRead;
        using (DeflateStream naive = new DeflateStream(new MemoryStream(withoutEnd), CompressionMode.Decompress))
            firstRead = naive.Read(new byte[cutRaw.Length], 0, cutRaw.Length);
        Check(firstRead == 0 || firstRead == cutRaw.Length,
            "块中途输入耗尽时第一次 Read 返回 0（托管解码器）或全部字节（zlib 解码器）");
        Console.WriteLine(firstRead == 0 ? "块中途输入耗尽：当前为托管解码器，覆盖第一次 Read 返回 0 的情形" :
            "块中途输入耗尽：当前为 zlib 解码器（托管解码器的情形需在未声明目标框架的程序中覆盖）");
        AcceptBlock(RawBlock(1, (uint)cutRaw.Length, 19, Crc(cutRaw), withoutEnd), cutRaw, FrameBlockLimit,
            "截掉块结束符号所在字节的块按 zlib 规则有效");
        AcceptBlock(RawBlock(1, (uint)cutRaw.Length, 20, Crc(cutRaw), full), cutRaw, FrameBlockLimit, "完整的手工 deflate 块有效");
        RejectBlock(RawBlock(1, (uint)cutRaw.Length, 18, Crc(cutRaw), full.Take(18).ToArray()), 38, FrameBlockLimit,
            "再截掉一个匹配后输出少于 rawBytes");
        RejectBlock(RawBlock(1, (uint)cutRaw.Length - 1, 19, Crc(cutRaw.Take(cutRaw.Length - 1).ToArray()), withoutEnd), 39,
            FrameBlockLimit, "块中途输入耗尽时仍能产生第 rawBytes+1 个字节");

        // 只前进的逻辑流：严格模式下块无效抛异常；恢复模式在首个无效块之前按正常结束返回。
        byte[] chain = DeflateBlock(compressible).Concat(StoredBlock(incompressible)).ToArray();
        byte[] expected = compressible.Concat(incompressible).ToArray();
        using (MemoryStream file = new MemoryStream(new byte[11].Concat(chain).ToArray()))
        using (MemoryStream output = new MemoryStream())
        {
            ReplayBlockStream blocks = new ReplayBlockStream(file, 11, chain.Length, false);
            blocks.CopyTo(output, 777);
            Check(output.ToArray().SequenceEqual(expected) && blocks.BlockCount == 2 && blocks.ValidDiskBytes == chain.Length &&
                blocks.LogicalBytes == expected.Length && blocks.Position == expected.Length && blocks.Read(new byte[1], 0, 1) == 0,
                "严格模式逐块解码并正好结束在段尾");
        }
        byte[] broken = chain.Concat(new byte[3]).ToArray();
        using (MemoryStream file = new MemoryStream(broken))
        {
            ReplayBlockStream blocks = new ReplayBlockStream(file, 0, broken.Length, false);
            Check(Fails(() => blocks.CopyTo(Stream.Null)) && blocks.Failure != null && Fails(() => blocks.ReadByte()),
                "严格模式段尾多余字节抛异常，之后继续报错");
        }
        using (MemoryStream file = new MemoryStream(broken))
        {
            ReplayBlockStream blocks = new ReplayBlockStream(file, 0, broken.Length, true);
            blocks.CopyTo(Stream.Null);
            Check(blocks.BlockCount == 2 && blocks.ValidDiskBytes == chain.Length && blocks.StopReason != null &&
                blocks.Failure == null, "恢复模式在段尾多余字节之前截止");
        }
        Console.WriteLine("容器 v3 块解码检查通过");
    }

    // 容器 v3 的完成文件与未完成录像。
    private static void RunContainerV3(string root, byte[] metadata)
    {
        string path = Path.Combine(root, "container-v3.yrrep");
        string partialPath = Path.Combine(root, "container-v3.yrrep.partial");
        byte[] s0 = CreateV4Frame(0, 2 | 4, 6, 0, 0);
        byte[] s1 = CreateV4Frame(1, 0, 0, 1, 0);
        byte[] s2 = CreateV4Frame(2, 4, 3, 2, 1);
        byte[] s3 = CreateV4Frame(3, 1 | 2 | 4, 2, 0, 0, 0, 101, 52, 9, 202, 52);
        byte[] s4 = CreateV4Frame(4, 1, 0, 0, 2);
        byte[] stream = CreateModeStream(5, 5, s0, s1, s2, s3, s4);
        int[] ends = Boundaries(s0, s1, s2, s3, s4);

        // 完成文件：单块、按帧边界切块、任意切点的两块、很多小块、帧流头跨越多块。
        Check(U32(Segment(stream), 4) == 1 && U32(Segment(stream, 5), 4) == 0, "测试帧流的块同时覆盖 method 1 与 method 0");
        AcceptV3(path, metadata, Segment(stream), stream, 5, 5, "v3 完成文件单块");
        AcceptV3(path, metadata, Segment(stream, ends.Take(4).ToArray()), stream, 5, 5, "v3 完成文件按帧边界切块");
        for (int cut = 1; cut < stream.Length; cut++)
            AcceptV3(path, metadata, Segment(stream, cut), stream, 5, 5, "v3 完成文件两块，切点 " + cut);
        AcceptV3(path, metadata, Segment(stream, Enumerable.Range(1, stream.Length / 7).Select(i => i * 7).ToArray()),
            stream, 5, 5, "v3 完成文件每 7 字节一块");
        AcceptV3(path, metadata, Segment(stream, 3, 6, 9, 12, 15, 18), stream, 5, 5, "v3 帧流头跨越多块，Open 继续读后续块");
        byte[] online = CreateModeStream(3, 1, CreateFrame(3, 0, 2));
        AcceptV3(path, metadata, Segment(online), online, 3, 1, "v3 容器装帧流版本 3 的联机录像");
        // 超过 64 KB 缓冲的事件载荷跨越多块：只前进的逻辑流读出丢弃。
        byte[] big = CreateModeStream(5, 5, s0, CreateV4Frame(1, 0, 0, 700, 0), CreateV4Frame(2, 0, 0, 0, 1));
        int[] bigCuts = Enumerable.Range(1, big.Length / 10000).Select(i => i * 10000).ToArray();
        AcceptV3(path, metadata, Segment(big, bigCuts), big, 5, 3, "v3 大事件载荷跨越多块");

        // 帧流头所在的块之后出错：完成文件 Open 照常，ValidatePayload 拒绝，帧数统计不给出数字；
        // 块恰好切在帧边界上，块错误也不能当作正常结束。未完成录像在该块之前截止，其后的有效块忽略。
        byte[] first = AutoBlock(stream.Take(ends[1]).ToArray());
        byte[] middleRaw = stream.Skip(ends[1]).Take(ends[3] - ends[1]).ToArray();
        byte[] middle = AutoBlock(middleRaw);
        byte[] last = AutoBlock(stream.Skip(ends[3]).ToArray());
        byte[] good = first.Concat(middle).Concat(last).ToArray();
        Check(U32(middle, 4) == 1 && good.SequenceEqual(Segment(stream, ends[1], ends[3])), "三块帧流段按帧边界切分");
        AcceptV3(path, metadata, good, stream, 5, 5, "v3 三块帧流段");
        foreach (KeyValuePair<string, byte[]> invalid in InvalidBlocks(middleRaw))
        {
            byte[] segment = first.Concat(invalid.Value).Concat(last).ToArray();
            RejectV3Later(path, metadata, segment, "v3 完成文件第二块" + invalid.Key);
            RecoverV3(partialPath, metadata, segment, 5, 2, first.Length, "v3 未完成录像在第二块（" + invalid.Key + "）之前截止");
        }
        foreach (KeyValuePair<string, byte[]> invalid in InvalidBlocks(stream.Take(ends[1]).ToArray()))
        {
            byte[] segment = invalid.Value.Concat(middle).Concat(last).ToArray();
            WriteV3Finished(path, metadata, segment);
            RejectOpen(path, "v3 完成文件首块" + invalid.Key + "时 Open 拒绝");
            RejectV3Partial(partialPath, metadata, segment, "v3 未完成录像首块" + invalid.Key + "即拒绝");
        }

        // 帧流段尾部多余字节、块链与 streamBytes 不符、完成文件尾部多余字节、容器头。
        Random random = new Random(3);
        byte[] noise = new byte[25];
        random.NextBytes(noise);
        foreach (byte[] tail in new[] { new byte[1], new byte[19], new byte[20], new byte[64], noise, StoredBlock(new byte[1]) })
            RejectV3Later(path, metadata, good.Concat(tail).ToArray(), "v3 帧流段尾部多余 " + tail.Length + " 字节");
        foreach (int missing in new[] { 1, 7, last.Length - 21, last.Length - 20, last.Length - 1 })
            RejectV3Later(path, metadata, good.Take(good.Length - missing).ToArray(), "v3 最后一块缺 " + missing + " 字节，块链超出 streamBytes");
        byte[] manifestBlock = AutoBlock(metadata);
        byte[] goodHash = Hash(manifestBlock.Concat(good).ToArray());
        WriteV3(path, manifestBlock, good.Concat(new byte[3]).ToArray(), 1, (ulong)good.Length, goodHash, (uint)manifestBlock.Length);
        Check(OpenError(path) == "回放长度或完成标志无效。", "v3 完成文件尾部多余字节");
        WriteV3(path, manifestBlock, good, 1, (ulong)good.Length + 1, goodHash, (uint)manifestBlock.Length);
        Check(OpenError(path) == "回放长度或完成标志无效。", "v3 streamBytes 超出文件");
        WriteV3(path, manifestBlock, good, 1, (ulong)good.Length, Hash(manifestBlock.Concat(good).Concat(new byte[1]).ToArray()),
            (uint)manifestBlock.Length);
        Check(Fails(ReplayArchive.Open(path).ValidatePayload), "v3 内容哈希不符");
        foreach (uint version in new uint[] { 0, 1, 4, uint.MaxValue })
        {
            WriteV3(path, manifestBlock, good, 1, (ulong)good.Length, goodHash, (uint)manifestBlock.Length, version);
            Check(OpenError(path) == "不支持的回放文件。", "容器版本 " + version + " 不支持");
        }
        foreach (uint flags in new uint[] { 2, uint.MaxValue })
        {
            WriteV3(path, manifestBlock, good, flags, (ulong)good.Length, goodHash, (uint)manifestBlock.Length);
            RejectOpen(path, "v3 flags=" + flags);
        }
        byte[] tiny = StoredBlock(new byte[] { 0x52 });
        WriteV3Finished(path, metadata, tiny);
        Check(OpenError(path) == "回放帧流头无效。", "v3 逻辑帧流不足 8 字节时 Open 拒绝");
        WriteV3(path, manifestBlock, tiny.Take(20).ToArray(), 1, 20, Hash(manifestBlock.Concat(tiny.Take(20)).ToArray()),
            (uint)manifestBlock.Length);
        Check(OpenError(path) == "回放长度或完成标志无效。", "v3 streamBytes 小于 21");
        WriteV3Finished(path, metadata, Segment(stream.Take(8).ToArray()));
        ReplayArchive eightBytes = ReplayArchive.Open(path);
        Check(eightBytes.StreamVersion == 5 && eightBytes.StreamGameMode == null && Fails(eightBytes.ValidatePayload),
            "v3 逻辑帧流只有 8 字节：Open 只取版本，ValidatePayload 拒绝");
        WriteV3Finished(path, metadata, Segment(stream.Take(20).ToArray(), 10));
        ReplayArchive shortHeader = ReplayArchive.Open(path);
        Check(shortHeader.StreamGameMode == 5u && Fails(shortHeader.ValidatePayload) && Fails(() => shortHeader.CountFrames()),
            "v3 逻辑帧流不足 32 字节：ValidatePayload 拒绝，帧数统计按帧流头无效抛出");
        RejectV3Later(path, metadata, Segment(Change(stream, ends[1], 9), ends[1]), "v3 逻辑帧流帧号跳跃");
        RejectV3Later(path, metadata, Segment(stream.Concat(new byte[3]).ToArray(), ends[4]), "v3 逻辑帧流尾部垃圾");
        RejectV3Later(path, metadata, Segment(stream.Take(ends[4] - 1).ToArray(), ends[2]), "v3 逻辑帧流在最后一帧中途结束");

        // 清单段：恰为一个有效块，块头加载荷正好占满 metadataBytes；解压结果必须是有效开局清单。
        WriteV3(path, manifestBlock.Concat(new byte[1]).ToArray(), good, 1, (ulong)good.Length,
            Hash(manifestBlock.Concat(new byte[1]).Concat(good).ToArray()), (uint)manifestBlock.Length + 1);
        RejectOpen(path, "v3 清单段在块之后多 1 字节");
        int half = metadata.Length / 2;
        byte[] twoManifestBlocks = AutoBlock(metadata.Take(half).ToArray()).Concat(AutoBlock(metadata.Skip(half).ToArray())).ToArray();
        WriteV3(path, twoManifestBlocks, good, 1, (ulong)good.Length, Hash(twoManifestBlocks.Concat(good).ToArray()),
            (uint)twoManifestBlocks.Length);
        RejectOpen(path, "v3 清单段由两个块组成");
        byte[] badManifest = (byte[])manifestBlock.Clone();
        badManifest[16] ^= 1;
        WriteV3(path, badManifest, good, 1, (ulong)good.Length, Hash(badManifest.Concat(good).ToArray()), (uint)badManifest.Length);
        RejectOpen(path, "v3 清单块 CRC 不符");
        byte[] notManifest = AutoBlock(new byte[100]);
        WriteV3(path, notManifest, good, 1, (ulong)good.Length, Hash(notManifest.Concat(good).ToArray()), (uint)notManifest.Length);
        RejectOpen(path, "v3 清单块解压结果不是有效开局清单");
        WriteV3(path, manifestBlock, good, 1, (ulong)good.Length, goodHash, 20);
        RejectOpen(path, "v3 metadataBytes 小于 21");
        WriteV3Partial(partialPath, metadata, good);
        byte[] partialBytes = File.ReadAllBytes(partialPath);
        File.WriteAllBytes(partialPath, partialBytes.Take(96 + manifestBlock.Length - 1).ToArray());
        RejectOpen(partialPath, "v3 未完成录像清单段被截断");

        // 未完成录像：完整恢复；逐字节截断候选帧流段时，在首个不完整的块之前截止，再在有效块的逻辑帧流上按帧截止。
        RecoverV3(partialPath, metadata, good, 5, 5, good.Length, "v3 未完成录像全部块有效");
        foreach (int[] cuts in new[] { new[] { ends[1], ends[3] }, new[] { ends[1] + 10, ends[3] - 5 }, new[] { 40 } })
        {
            byte[] segment = Segment(stream, cuts);
            int[] diskEnds = BlockEnds(segment);
            int[] logicalEnds = cuts.Concat(new[] { stream.Length }).ToArray();
            string label = "v3 未完成录像（切点 " + string.Join(",", cuts) + "）截断到 ";
            for (int length = 0; length <= segment.Length; length++)
            {
                int blocks = diskEnds.Count(end => end <= length);
                int frames = blocks == 0 ? 0 : ends.Count(end => end <= logicalEnds[blocks - 1]);
                byte[] candidate = segment.Take(length).ToArray();
                if (frames == 0) RejectV3Partial(partialPath, metadata, candidate, label + length + " 字节时没有可恢复帧");
                else RecoverV3(partialPath, metadata, candidate, 5, (uint)frames, diskEnds[blocks - 1], label + length + " 字节");
            }
        }
        foreach (byte[] tail in new[] { new byte[3], new byte[19], new byte[20], new byte[64], noise })
            RecoverV3(partialPath, metadata, good.Concat(tail).ToArray(), 5, 5, good.Length, "v3 未完成录像尾部 " + tail.Length + " 字节被忽略");
        // 有效块内的帧级截止：其后的块仍有效，计入有效块前缀。
        byte[] badSpeed = CreateModeStream(5, 5, s0, s1, CreateV4Frame(2, 4, 7, 0, 0), s3, s4);
        byte[] badSpeedSegment = Segment(badSpeed, ends[0], ends[1] + 10);
        RecoverV3(partialPath, metadata, badSpeedSegment, 5, 2, badSpeedSegment.Length, "v3 未完成录像有效块内的无效帧处截止");
        byte[] bigSegment = Segment(big, bigCuts);
        int[] bigDiskEnds = BlockEnds(bigSegment);
        RecoverV3(partialPath, metadata, bigSegment.Take(bigDiskEnds[bigDiskEnds.Length - 1] - 1).ToArray(), 5, 1,
            bigDiskEnds[bigDiskEnds.Length - 2], "v3 未完成录像大事件载荷所在的最后一块不完整");
        RejectV3Partial(partialPath, metadata, new byte[0], "v3 未完成录像没有任何块");
        RejectV3Partial(partialPath, metadata, Segment(stream.Take(31).ToArray()), "v3 未完成录像逻辑帧流不足 32 字节");
        RejectV3Partial(partialPath, metadata, Segment(stream.Take(32).ToArray()), "v3 未完成录像只有帧流头");
        RejectV3Partial(partialPath, metadata, Segment(Change(stream, 4, 6)), "v3 未完成录像帧流版本 6");
        WriteV3(partialPath, manifestBlock, good, 0, (ulong)good.Length, new byte[32], (uint)manifestBlock.Length);
        RejectOpen(partialPath, "v3 flags=0 但 streamBytes 非零");
        WriteV3(partialPath, manifestBlock, good, 0, 0, goodHash, (uint)manifestBlock.Length);
        RejectOpen(partialPath, "v3 flags=0 但内容哈希非零");

        // 打开之后文件被改动：有效块前缀、可恢复帧数或清单变化都必须被发现；无法恢复的尾部变化不影响。
        byte[] twoBlocks = first.Concat(middle).ToArray();
        WriteV3Partial(partialPath, metadata, twoBlocks);
        ReplayArchive recovered = ReplayArchive.Open(partialPath);
        Check(recovered.RecoveredFrameCount == 4 && recovered.StreamBytes == twoBlocks.Length, "v3 未完成录像恢复前两块");
        byte[] twoBlockFile = File.ReadAllBytes(partialPath);
        File.WriteAllBytes(partialPath, twoBlockFile.Concat(last.Take(last.Length / 2)).ToArray());
        recovered.ValidatePayload();
        Check(recovered.MatchesCurrentHeader(), "v3 追加不完整的块不改变可恢复内容");
        File.WriteAllBytes(partialPath, twoBlockFile.Concat(last).ToArray());
        Check(Fails(recovered.ValidatePayload) && !recovered.MatchesCurrentHeader(), "v3 追加有效块被发现");
        File.WriteAllBytes(partialPath, twoBlockFile.Take(twoBlockFile.Length - 1).ToArray());
        Check(Fails(recovered.ValidatePayload) && !recovered.MatchesCurrentHeader(), "v3 有效块被截断被发现");
        byte[] changedMetadata = (byte[])metadata.Clone();
        changedMetadata[8] ^= 1; // RecordedAtUnixSeconds 最低字节，清单仍可解析
        WriteV3Partial(partialPath, changedMetadata, twoBlocks);
        Check(Fails(recovered.ValidatePayload) && !recovered.MatchesCurrentHeader(), "v3 清单变化被发现");
        File.WriteAllBytes(partialPath, twoBlockFile);
        recovered.ValidatePayload();
        Check(recovered.MatchesCurrentHeader(), "v3 恢复原文件后再次一致");
        WriteV3Finished(path, metadata, good);
        ReplayArchive finishedArchive = ReplayArchive.Open(path);
        WriteV3Partial(path, metadata, good);
        Check(Fails(finishedArchive.ValidatePayload) && !finishedArchive.MatchesCurrentHeader(), "v3 完成文件变为未完成录像被发现");
        WriteArchive(path, metadata, stream);
        Check(ReplayArchive.Open(path).ContainerVersion == 2 && Fails(finishedArchive.ValidatePayload) &&
            !finishedArchive.MatchesCurrentHeader(), "v3 完成文件换成同一帧流的 v2 被发现");

        // 1 GiB 上限：候选帧流段（磁盘）恰为 1 GiB 时可恢复，多 1 字节直接拒绝。
        long gibibyte = 1024L * 1024 * 1024;
        WriteV3Partial(partialPath, metadata, good);
        using (FileStream large = new FileStream(partialPath, FileMode.Open, FileAccess.Write, FileShare.None))
            large.SetLength(96 + manifestBlock.Length + gibibyte);
        ReplayArchive diskLimit = ReplayArchive.Open(partialPath);
        Check(diskLimit.RecoveredFrameCount == 5 && diskLimit.StreamBytes == good.Length, "v3 候选帧流段恰为 1 GiB 时在零尾部之前截止");
        using (FileStream large = new FileStream(partialPath, FileMode.Open, FileAccess.Write, FileShare.None))
            large.SetLength(96 + manifestBlock.Length + gibibyte + 1);
        RejectOpen(partialPath, "v3 候选帧流段超过 1 GiB 直接拒绝");

        // 逻辑帧流 1 GiB 上限：先判定块是否有效，只有有效块才累计；累计超过 1 GiB 即拒绝整份文件。
        byte[] zeroBlock = DeflateBlock(new byte[FrameBlockLimit]);
        List<byte> atLimit = new List<byte>(AutoBlock(stream));
        for (int i = 0; i < 255; i++) atLimit.AddRange(zeroBlock);
        atLimit.AddRange(DeflateBlock(new byte[FrameBlockLimit - stream.Length]));
        byte[] invalidZeroBlock = (byte[])zeroBlock.Clone();
        invalidZeroBlock[16] ^= 1;
        WriteV3Partial(partialPath, metadata, atLimit.ToArray());
        ReplayArchive logicalLimit = ReplayArchive.Open(partialPath);
        Check(logicalLimit.RecoveredFrameCount == 5 && logicalLimit.StreamBytes == atLimit.Count, "v3 有效块逻辑帧流恰为 1 GiB 时可恢复");
        WriteV3Partial(partialPath, metadata, atLimit.Concat(invalidZeroBlock).ToArray());
        Check(ReplayArchive.Open(partialPath).StreamBytes == atLimit.Count, "v3 超出 1 GiB 的块 CRC 不符时不计入，照常恢复");
        WriteV3Partial(partialPath, metadata, atLimit.Concat(RawBlock(0, FrameBlockLimit, FrameBlockLimit, 0, new byte[10])).ToArray());
        Check(ReplayArchive.Open(partialPath).StreamBytes == atLimit.Count, "v3 超出 1 GiB 的块载荷不完整时不计入，照常恢复");
        WriteV3Partial(partialPath, metadata, atLimit.Concat(StoredBlock(new byte[1])).ToArray());
        RejectOpen(partialPath, "v3 有效块逻辑帧流超过 1 GiB 即拒绝整份文件");
        byte[] overLimit = atLimit.Concat(StoredBlock(new byte[1])).ToArray();
        using (MemoryStream file = new MemoryStream(atLimit.ToArray()))
            new ReplayBlockStream(file, 0, atLimit.Count, false).Drain();
        checks++;
        using (MemoryStream file = new MemoryStream(overLimit))
        {
            ReplayBlockStream blocks = new ReplayBlockStream(file, 0, overLimit.Length, false);
            Check(Fails(blocks.Drain) && blocks.Failure != null && blocks.LogicalBytes == gibibyte,
                "严格模式逻辑帧流超过 1 GiB 时抛异常");
        }
        File.Delete(path);
        File.Delete(partialPath);
        Console.WriteLine("容器 v3 完成文件与未完成录像检查通过");
    }

    // 插件写出的容器 v3 样例（tests/fixtures）：帧 0–5 与 replay-v5-speed.yrrep 相同，
    // 以测试用切块策略（每块最多 3 个单元）写成 3 块：帧流头加帧 0–1、帧 2–4、帧 5。
    // 未完成样例写入帧 0–4 后中止（前两块已写出），再追加完成样例第三块磁盘字节的前一半；可恢复帧 0–4。
    // 先逐字段核对容器头、清单块与各块，再把帧流段原样换上有效清单交给 ReplayArchive（样例清单不一定是有效开局清单）。
    private static void RunContainerV3Fixtures(string root, byte[] metadata, string fixtureDirectory)
    {
        string finishedPath = Path.Combine(fixtureDirectory, "replay-c3-v5-speed.yrrep");
        string partialPath = Path.Combine(fixtureDirectory, "replay-c3-v5-speed-partial.yrrep");
        foreach (string required in new[] { finishedPath, partialPath })
        {
            if (File.Exists(required)) continue;
            string missing = "缺少插件容器 v3 跨语言样例：" + required +
                "。样例由插件的 ReplayArchiveTests --generate 生成，不能跳过。";
            Console.Error.WriteLine("FAIL " + missing);
            throw new Exception(missing);
        }
        byte[] finished = File.ReadAllBytes(finishedPath);
        byte[] partial = File.ReadAllBytes(partialPath);
        // 核对长度、SHA-256 与块布局。
        Check(finished.Length == 306 && HexHash(finished) == "e9fc36e2374cb7a8038018727147c6c94d97e4940a01dae44538ee92d8309179" &&
            BitConverter.ToString(finished, 64, 32).Replace("-", "").ToLowerInvariant() ==
                "0f86b8348a6b3b538b0c84418fa4885c16bd9df08f2212626cf1b5f36fc9d995",
            "插件 c3 完成样例长度、SHA-256 与内容哈希");
        Check(partial.Length == 283 && HexHash(partial) == "b1122396fb56055e00aab1e55b0fbf90b8b7ea486d54a433f914cf41b1c611e7",
            "插件 c3 未完成样例长度与 SHA-256");
        byte[] v5 = File.ReadAllBytes(Path.Combine(fixtureDirectory, "replay-v5-speed.yrrep"));
        byte[] v5Stream = v5.Skip(96 + (int)U32(v5, 16)).ToArray();
        byte[] gameHash = Enumerable.Range(1, 32).Select(value => (byte)value).ToArray();
        uint metadataBytes = U32(finished, 16);
        foreach (byte[] file in new[] { finished, partial })
            Check(Encoding.ASCII.GetString(file, 0, 8) == "RAREPL2\0" && U32(file, 8) == 3 && U32(file, 12) == 96 &&
                U32(file, 16) == metadataBytes && file.Skip(32).Take(32).SequenceEqual(gameHash), "插件 c3 样例容器头");
        ulong streamBytes = BitConverter.ToUInt64(finished, 24);
        Check(U32(finished, 20) == 1 && (ulong)finished.Length == 96 + metadataBytes + streamBytes &&
            Hash(finished.Skip(96).ToArray()).SequenceEqual(finished.Skip(64).Take(32)),
            "插件 c3 完成样例：flags=1、文件长度与内容哈希");
        Check(U32(partial, 20) == 0 && BitConverter.ToUInt64(partial, 24) == 0 && partial.Skip(64).Take(32).All(value => value == 0) &&
            partial.Length > 96 + metadataBytes &&
            partial.Skip(96).Take((int)metadataBytes).SequenceEqual(finished.Skip(96).Take((int)metadataBytes)),
            "插件 c3 未完成样例：flags=0、零长度与零哈希，清单段与完成样例相同");
        ReplayBlockHeader header;
        byte[] fixtureMetadata;
        Check(ReplayBlocks.TryDecodeBlock(finished, 96, (int)metadataBytes, ReplayBlocks.ManifestMaxRawBytes, out header,
            out fixtureMetadata) && header.DiskBytes == metadataBytes, "插件 c3 样例清单段恰为一个有效块");
        Check(metadataBytes == 25 && header.Method == 0 && header.Crc32 == 0x3171E593u &&
            fixtureMetadata.SequenceEqual(new byte[] { 0x41, 0x00, 0xFE, 0x42, 0x43 }), "插件 c3 样例清单块：method 0、5 字节占位清单");

        byte[] segment = finished.Skip(96 + (int)metadataBytes).ToArray();
        List<int> diskEnds = new List<int>();
        List<int> rawSizes = new List<int>();
        List<uint> methods = new List<uint>();
        List<uint> storedSizes = new List<uint>();
        List<uint> crcs = new List<uint>();
        List<byte> logical = new List<byte>();
        int offset = 0;
        while (offset < segment.Length)
        {
            byte[] raw;
            Check(ReplayBlocks.TryDecodeBlock(segment, offset, segment.Length - offset, ReplayBlocks.FrameStreamMaxRawBytes,
                out header, out raw), "插件 c3 完成样例块 " + diskEnds.Count + " 有效");
            logical.AddRange(raw);
            offset += (int)header.DiskBytes;
            diskEnds.Add(offset);
            rawSizes.Add(raw.Length);
            methods.Add(header.Method);
            storedSizes.Add(header.StoredBytes);
            crcs.Add(header.Crc32);
        }
        Check(diskEnds.Count == 3 && rawSizes.SequenceEqual(new[] { 219, 116, 147 }),
            "插件 c3 完成样例 3 块：帧流头加帧 0–1、帧 2–4、帧 5（解压长度 " + string.Join(",", rawSizes) + "）");
        Check(diskEnds.SequenceEqual(new[] { 75, 139, 185 }) && methods.All(method => method == 1) &&
            storedSizes.SequenceEqual(new uint[] { 55, 44, 26 }) && crcs.SequenceEqual(new uint[] { 0x5EEAAB7A, 0x223FA013, 0x467749F0 }),
            "插件 c3 完成样例各块均为 method 1，载荷长度与 CRC-32 符合预期");
        Check(logical.SequenceEqual(v5Stream), "插件 c3 样例逻辑帧流与 v5 样例帧流逐字节相同");
        using (MemoryStream file = new MemoryStream(segment))
        using (MemoryStream output = new MemoryStream())
        {
            ReplayBlockStream blocks = new ReplayBlockStream(file, 0, segment.Length, false);
            blocks.CopyTo(output);
            Check(output.ToArray().SequenceEqual(v5Stream) && blocks.BlockCount == 3 && blocks.ValidDiskBytes == segment.Length,
                "插件 c3 样例经逻辑流解码与 v5 样例帧流相同");
        }
        byte[] candidate = partial.Skip(96 + (int)metadataBytes).ToArray();
        int third = diskEnds[2] - diskEnds[1];
        int appended = candidate.Length - diskEnds[1];
        Check(candidate.SequenceEqual(segment.Take(candidate.Length)) && third == 46 && appended == 23,
            "插件 c3 未完成样例候选段为前两块加第三块磁盘字节的前一半（23 字节）");

        string path = Path.Combine(root, "fixture-c3.yrrep");
        string partialCopy = Path.Combine(root, "fixture-c3.yrrep.partial");
        AcceptV3(path, metadata, segment, v5Stream, 5, 6, "插件 c3 完成样例换上有效清单后通过校验");
        RecoverV3(partialCopy, metadata, candidate, 5, 5, diskEnds[1], "插件 c3 未完成样例恢复 5 帧");
        // 逐字节截断完成样例的帧流段：块级截止叠加帧级截止。
        int[] frameEnds = { 84, 219, 283, 311, 335, 482 };
        for (int length = 0; length <= segment.Length; length++)
        {
            int blocks = diskEnds.Count(end => end <= length);
            int logicalEnd = rawSizes.Take(blocks).Sum();
            int frames = frameEnds.Count(end => end <= logicalEnd);
            byte[] prefix = segment.Take(length).ToArray();
            if (frames == 0) RejectV3Partial(partialCopy, metadata, prefix, "插件 c3 样例截断到 " + length + " 字节时没有可恢复帧");
            else RecoverV3(partialCopy, metadata, prefix, 5, (uint)frames, diskEnds[blocks - 1], "插件 c3 样例截断到 " + length + " 字节");
        }
        File.Delete(path);
        File.Delete(partialCopy);
        Console.WriteLine("插件 c3 完成样例 SHA-256 " + HexHash(finished) + "（" + finished.Length + " 字节），未完成样例 SHA-256 " +
            HexHash(partial) + "（" + partial.Length + " 字节），清单 " + fixtureMetadata.Length + " 字节");
        Console.WriteLine("插件容器 v3 样例核对通过");
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
        // 版本 4、5 已是有效版本（见 RunGameSpeed），未知版本改用 6 与最大值。
        Reject(archivePath, metadata, Change(valid, 4, 6), "未知版本");
        Accept(archivePath, metadata, Change(valid, 4, 5), 5, "V3 联机帧流改为版本 5 按 V4 规则接受");
        Reject(archivePath, metadata, Change(valid, 4, uint.MaxValue), "版本为最大值");
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
        RunUnfinalized(root, metadata, fixtureDirectory);
        RunGameSpeed(root, metadata, 4);
        RunGameSpeed(root, metadata, 5);
        RunStreamHeaderOpen(root, metadata);
        RunBlockCodec();
        RunContainerV3(root, metadata);
        if (!string.IsNullOrEmpty(fixtureDirectory))
        {
            RunGameSpeedFixtures(root, metadata, fixtureDirectory, 4);
            // v5 样例由插件测试生成：两份都不在时说明未核对；只要有一份就完整核对（缺另一份即失败）。
            if (File.Exists(Path.Combine(fixtureDirectory, "replay-v5-speed.yrrep")) ||
                File.Exists(Path.Combine(fixtureDirectory, "replay-v5-speed-partial.yrrep")))
                RunGameSpeedFixtures(root, metadata, fixtureDirectory, 5);
            else
                Console.WriteLine("样例目录中没有插件 v5 游戏速度样例，未核对");
            // 容器 v3 样例必须存在：缺少时明确失败，不能静默跳过（放在最后，其余检查先全部执行）。
            RunContainerV3Fixtures(root, metadata, fixtureDirectory);
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
