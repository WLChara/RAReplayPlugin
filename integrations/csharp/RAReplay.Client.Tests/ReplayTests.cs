using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Linq;
using System.Text;
using RAReplay.Client;

internal static class ReplayTests
{
    private static int checks;

    private static void Check(bool valid, string name)
    {
        if (!valid)
        {
            Console.Error.WriteLine("FAIL " + name);
            throw new Exception(name);
        }
        checks++;
    }

    private static void Reject<T>(Action action, string name) where T : Exception
    {
        try { action(); }
        catch (T) { checks++; return; }
        throw new Exception(name);
    }

    private static byte[] Hash(byte[] bytes)
    {
        using (SHA256 sha = SHA256.Create()) return sha.ComputeHash(bytes);
    }

    private static byte[] Join(byte[] first, byte[] second)
    {
        byte[] joined = new byte[first.Length + second.Length];
        Buffer.BlockCopy(first, 0, joined, 0, first.Length);
        Buffer.BlockCopy(second, 0, joined, first.Length, second.Length);
        return joined;
    }

    private static void Archive(string path, byte[] manifest, byte[] frameStream, byte[] gameHash = null)
    {
        using (BinaryWriter writer = new BinaryWriter(File.Create(path)))
        {
            writer.Write(Encoding.ASCII.GetBytes("RAREPL2\0"));
            writer.Write(2u);
            writer.Write(96u);
            writer.Write((uint)manifest.Length);
            writer.Write(1u);
            writer.Write((ulong)frameStream.Length);
            writer.Write(gameHash ?? new byte[32]);
            writer.Write(Hash(Join(manifest, frameStream)));
            writer.Write(manifest);
            writer.Write(frameStream);
        }
    }

    // 录制异常中止遗留的容器：flags=0、streamBytes=0、内容哈希全零，metadata 与已追加的帧流原样写入。
    private static void Partial(string path, byte[] manifest, byte[] frameStream, byte[] gameHash = null)
    {
        using (BinaryWriter writer = new BinaryWriter(File.Create(path)))
        {
            writer.Write(Encoding.ASCII.GetBytes("RAREPL2\0"));
            writer.Write(2u);
            writer.Write(96u);
            writer.Write((uint)manifest.Length);
            writer.Write(0u);
            writer.Write(0ul);
            writer.Write(gameHash ?? Enumerable.Repeat((byte)0x22, 32).ToArray());
            writer.Write(new byte[32]);
            writer.Write(manifest);
            writer.Write(frameStream);
        }
    }

    private static byte[] Frames(uint frameNumber, uint count, uint flags,
        uint streamVersion = 2, uint effectCount = 0)
    {
        using (MemoryStream output = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(0x32465052u);
            writer.Write(streamVersion);
            writer.Write(111u);
            writer.Write(0u);
            writer.Write(0u);
            writer.Write(0u);
            writer.Write(3u);
            writer.Write(123u);
            writer.Write(frameNumber);
            writer.Write(0u);
            writer.Write(count);
            writer.Write(flags);
            if (streamVersion == 2) writer.Write(effectCount);
            return output.ToArray();
        }
    }

    private static byte[] Effect(uint kind = 1, uint reverse = 0)
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

    // 版本 3–5 帧流（帧流头 gameMode 可指定）：每个元素一条空记录，帧 0 带空编队快照；
    // 元素有值时置 flags bit2 并在帧头后写入该游戏速度（版本 3 置 bit2、版本 6 用于构造无效记录）。
    private static byte[] SpeedStream(uint streamVersion, uint gameMode, params uint?[] speeds)
    {
        using (MemoryStream output = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(0x32465052u);
            writer.Write(streamVersion);
            writer.Write(111u);
            writer.Write(gameMode);
            writer.Write(0u);
            writer.Write(0u);
            writer.Write(3u);
            writer.Write(123u);
            for (int frame = 0; frame < speeds.Length; frame++)
            {
                writer.Write((uint)frame);
                writer.Write(0u);
                writer.Write(0u);
                writer.Write((frame == 0 ? 2u : 0u) | (speeds[frame].HasValue ? 4u : 0u));
                writer.Write(0u);
                writer.Write(0u);
                if (speeds[frame].HasValue) writer.Write(speeds[frame].Value);
            }
            return output.ToArray();
        }
    }

    // 单条 v2 帧记录：eventCount 个全零事件、effectCount 个输入效果；effectKind!=1 用于构造无效记录。
    private static byte[] Record(uint frame, uint eventCount, uint effectCount, uint effectKind = 1)
    {
        using (MemoryStream output = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(frame);
            writer.Write(0u);
            writer.Write(eventCount);
            writer.Write(0u);
            writer.Write(effectCount);
            writer.Write(new byte[eventCount * 111]);
            for (uint i = 0; i < effectCount; i++) writer.Write(Effect(effectKind));
            return output.ToArray();
        }
    }

    // 以独立的“记录末尾偏移”为参照，核对缓冲式帧扫描在 64 KB 缓冲边界、超过缓冲的大事件载荷与任意截断处的结果。
    private static void BufferedScanTests(string root, byte[] encoded)
    {
        byte[] header = Frames(0, 0, 0, 2, 0).Take(32).ToArray();
        const int FrameCount = 6000;
        const int BigFrame = 2500; // 700 个事件 = 77700 字节，超过 64 KB 缓冲
        System.Collections.Generic.List<byte[]> records = new System.Collections.Generic.List<byte[]>();
        long[] ends = new long[FrameCount];
        long position = header.Length;
        for (uint frame = 0; frame < FrameCount; frame++)
        {
            uint events = frame == BigFrame ? 700u : (frame % 7 == 0 ? 1u + frame % 3 : 0u);
            uint effects = frame % 11 == 0 ? 1u + frame % 2 : 0u;
            byte[] record = Record(frame, events, effects);
            records.Add(record);
            position += record.Length;
            ends[frame] = position;
        }
        byte[] stream = header.Concat(records.SelectMany(record => record)).ToArray();
        Check(stream.Length == position && stream.Length > 3 * 64 * 1024, "scan test stream spans several buffers");
        string path = Path.Combine(root, "buffered-scan.yrrep.partial");

        Partial(path, encoded, stream);
        ReplayArchive full = ReplayArchive.Open(path);
        Check(full.IsUnfinalized && full.RecoveredFrameCount == FrameCount && full.StreamBytes == stream.Length,
            "buffered scan recovers every frame across buffers and a large payload");
        full.ValidatePayload();
        checks++;

        // 截断位置：缓冲边界附近、大载荷两端、若干记录末尾的前后一字节，以及固定种子的随机位置。
        System.Collections.Generic.SortedSet<long> cuts = new System.Collections.Generic.SortedSet<long>();
        foreach (long center in new[] { 32L + 65536, 32L + 2 * 65536, 32L + 3 * 65536, ends[BigFrame - 1], ends[BigFrame] })
            for (long delta = -48; delta <= 48; delta++) cuts.Add(center + delta);
        foreach (int frame in new[] { 0, 1, 2, 10, 11, 77, 78, BigFrame - 1, BigFrame, BigFrame + 1, 4095, 4096, FrameCount - 2, FrameCount - 1 })
            foreach (long delta in new[] { -1L, 0L, 1L }) cuts.Add(ends[frame] + delta);
        Random random = new Random(20261019);
        for (int i = 0; i < 150; i++) cuts.Add(random.Next((int)ends[0], stream.Length + 1));
        int cutChecks = 0;
        foreach (long cut in cuts)
        {
            if (cut < ends[0] || cut > stream.Length) continue;
            int expectedFrames = 0;
            while (expectedFrames < FrameCount && ends[expectedFrames] <= cut) expectedFrames++;
            Partial(path, encoded, stream.Take((int)cut).ToArray());
            ReplayArchive cutArchive = ReplayArchive.Open(path);
            if (cutArchive.RecoveredFrameCount != expectedFrames || cutArchive.StreamBytes != ends[expectedFrames - 1])
                throw new Exception("buffered scan truncation at " + cut + ": recovered " + cutArchive.RecoveredFrameCount +
                    " bytes " + cutArchive.StreamBytes + ", expected " + expectedFrames + " bytes " + ends[expectedFrames - 1]);
            cutArchive.ValidatePayload();
            cutChecks++;
        }
        checks += cutChecks;
        Check(cutChecks > 400, "buffered scan truncation cases executed");

        // 第一个无效记录（输入效果 kind 非法）之后的内容忽略；位置选在 64 KB 缓冲边界与大载荷之后。
        int invalidAt = 2600;
        byte[] withInvalid = header.Concat(records.Select((record, index) => index == invalidAt ? Record((uint)index, 0, 1, 2) : record)
            .SelectMany(record => record)).ToArray();
        Partial(path, encoded, withInvalid);
        ReplayArchive invalid = ReplayArchive.Open(path);
        Check(invalid.RecoveredFrameCount == invalidAt && invalid.StreamBytes == ends[invalidAt - 1],
            "invalid record after a large payload ends recovery before it");
        invalid.ValidatePayload();
        checks++;

        // 完成文件：同一份帧流必须通过严格逐帧校验；改坏同一条记录则被拒绝。
        Archive(path, encoded, stream);
        ReplayArchive.Open(path).ValidatePayload();
        checks++;
        Archive(path, encoded, withInvalid);
        Reject<InvalidDataException>(() => ReplayArchive.Open(path).ValidatePayload(), "finished archive rejects the invalid record");
        File.Delete(path);
    }

    // OpenAll：按最后写入时间倒序，跳过损坏、被独占与后缀不符的文件；目录不存在或为空返回空列表。
    private static void OpenAllTests(string root, byte[] encoded, byte[] frames, byte[] partialStream)
    {
        string directory = Path.Combine(root, "OpenAll");
        Check(ReplayArchive.OpenAll(directory).Count == 0, "OpenAll of a missing directory is empty");
        Directory.CreateDirectory(directory);
        Check(ReplayArchive.OpenAll(directory).Count == 0, "OpenAll of an empty directory is empty");
        string older = Path.Combine(directory, "older.yrrep");
        string middle = Path.Combine(directory, "middle.yrrep");
        string newest = Path.Combine(directory, "newest.yrrep.partial");
        string garbage = Path.Combine(directory, "garbage.yrrep");
        string locked = Path.Combine(directory, "locked.yrrep.partial");
        string wrongSuffix = Path.Combine(directory, "ignored.yrrep.bak");
        Archive(older, encoded, frames);
        Archive(middle, encoded, frames);
        Partial(newest, encoded, partialStream);
        File.WriteAllBytes(garbage, new byte[200]);
        Partial(locked, encoded, partialStream);
        Archive(wrongSuffix, encoded, frames);
        DateTime start = new DateTime(2026, 10, 1, 0, 0, 0, DateTimeKind.Utc);
        File.SetLastWriteTimeUtc(older, start);
        File.SetLastWriteTimeUtc(middle, start.AddDays(4));
        File.SetLastWriteTimeUtc(newest, start.AddDays(9));
        File.SetLastWriteTimeUtc(locked, start.AddDays(10));
        File.SetLastWriteTimeUtc(garbage, start.AddDays(11));
        File.SetLastWriteTimeUtc(wrongSuffix, start.AddDays(12));
        using (new FileStream(locked, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
        {
            string[] names = ReplayArchive.OpenAll(directory).Select(archive => Path.GetFileName(archive.Path)).ToArray();
            Check(names.SequenceEqual(new[] { "newest.yrrep.partial", "middle.yrrep", "older.yrrep" }),
                "OpenAll sorts newest first and skips corrupt, locked and wrong-suffix files");
        }
        string[] all = ReplayArchive.OpenAll(directory).Select(archive => Path.GetFileName(archive.Path)).ToArray();
        Check(all.SequenceEqual(new[] { "locked.yrrep.partial", "newest.yrrep.partial", "middle.yrrep", "older.yrrep" }),
            "OpenAll includes the partial once the writer releases it");
        Directory.Delete(directory, true);
    }

    // 精简的客户端 GameOptions.ini：阵营、颜色、下拉框（含 Index 与 MapCode 写入方式）、复选框（含反转与隐藏项）。
    private const string SampleGameOptions =
        "[General]\r\nSides=美国,欧洲联盟,太平洋阵线,苏俄,拉丁同盟,中国,心灵军团,天蝎组织,总部守卫,狂鲨先锋,科洛尼亚侧翼,最后堡垒\r\n" +
        "[MPColors]\r\n墨绿色=0,168,168,0\r\n红色=255,0,0,1\r\n青色=0,255,255,2\r\n浅绿色=1,255,0,3\r\n紫色=180,91,229,4\r\n" +
        "黄色=255,216,0,5\r\n蓝色=0,150,255,6\r\n橙色=255,132,0,7\r\n紫红色=223,0,255,8\r\n棕色=178,103,50,9\r\n绿色=119,190,38,10\r\n" +
        "深红色=178,0,66,11\r\n天蓝色=145,180,255,12\r\n" +
        "[MultiplayerGameLobby]\r\nCheckBoxes=chkShortGame,chkNavalCombat,chkNoTag,chkRevCrates,chkDEVNerfEights\r\n" +
        "DropDowns=cmbUnitCount,cmbGameSpeedCap,cmbCredits,cmbOreGrowth,cmbNoLabel\r\n" +
        "[SkirmishLobby]\r\nDropDowns=cmbUnitCount,cmbSkirmishGameSpeedCap,cmbCredits\r\n" +
        "[lblUnitCount]\r\nText=初始部队数\r\n[cmbUnitCount]\r\nItems=50,40,30,20,10,1,0\r\nSpawnIniOption=UnitCount\r\nDataWriteMode=String\r\n" +
        "[lblGameSpeedCap]\r\nText=游戏速度\r\n[cmbGameSpeedCap]\r\nItems=6 最快,5 很快,4 快,3 中等,2 慢,1 很慢,0 最慢\r\n" +
        "SpawnIniOption=GameSpeed\r\nDataWriteMode=Index\t\t;注释\r\n" +
        "[cmbSkirmishGameSpeedCap]\r\nItems=6 最快,5 很快\r\nSpawnIniOption=GameSpeed\r\nDataWriteMode=Index\r\n" +
        "[lblCredits]\r\nText=初始资金\r\n[cmbCredits]\r\nItems=50000,40000,30000,20000,10000,7500,5000\r\nSpawnIniOption=Credits\r\nDataWriteMode=String\r\n" +
        "[lblOreGrowth]\r\nText=矿石生长\r\n[cmbOreGrowth]\r\nItems=a.ini,b.ini\r\nItemLabels=标准,双倍\r\nDataWriteMode=MapCode\r\n" +
        "[cmbNoLabel]\r\nItems=1,2\r\nSpawnIniOption=NoLabelKey\r\nDataWriteMode=String\r\n" +
        "[chkShortGame]\r\nText=快速游戏\r\nSpawnIniOption=ShortGame\r\n" +
        "[chkNavalCombat]\r\nText=禁止海军单位\r\nSpawnIniOption=NavalCombat\r\n" +
        "[chkNoTag]\r\nSpawnIniOption=NoTagKey\r\n" +
        "[chkRevCrates]\r\nText=无工具箱\r\nSpawnIniOption=Crates\r\nReversed=yes\r\n" +
        "[chkDEVNerfEights]\r\nSpawnIniOption=NerfEights\r\nVisible=0\r\n";

    // 按真实录像的 spawn.ini 格式构造（4 名真人，录制者与另外三人；玩家名与地址均为示例值）。
    private const string SampleSpawnFourHumans =
        "[Settings]\nName=PlayerA\nScenario=spawnmap.ini\nUIGameMode=常规作战\nUIMapName=(4) 西伯利亚 · 木\nPlayerCount=4\nSide=4\n" +
        "IsSpectator=False\nColor=3\nAIPlayers=0\nSeed=167332710\nShortGame=True\nNavalCombat=False\nCrates=False\nUnitCount=0\nGameSpeed=0\n" +
        "Credits=10000\nTechLevel=10\nFogOfWar=No\n[Tunnel]\nIp=203.0.113.10\nPort=50000\n" +
        "[Other1]\nName=PlayerB\nSide=8\nIsSpectator=False\nColor=4\nIp=0.0.0.0\nPort=22759\n" +
        "[Other2]\nName=PlayerC\nSide=5\nIsSpectator=False\nColor=8\nIp=0.0.0.0\nPort=10078\n" +
        "[Other3]\nName=PlayerD\nSide=6\nIsSpectator=False\nColor=11\nIp=0.0.0.0\nPort=-12434\n" +
        "[SpawnLocations]\nMulti1=0\nMulti2=2\n";

    // 构造样例：录制者 + 观察者 + 两个电脑，两人同盟（按 AllianceHolder 的写法）。
    private const string SampleSpawnAiAndAlliance =
        "[Settings]\nName=Me\nUIGameMode=测试模式\nPlayerCount=2\nSide=1\nIsSpectator=False\nColor=5\nAIPlayers=2\n" +
        "UnitCount=1\nGameSpeed=1\nCredits=7500\nShortGame=False\nNavalCombat=True\nCrates=True\nMystery=1\n" +
        "[Other1]\nName=Spec\nSide=0\nIsSpectator=True\nColor=2\n" +
        "[HouseHandicaps]\nMulti3=0\nMulti4=2\n[HouseCountries]\nMulti3=3\nMulti4=99\n[HouseColors]\nMulti3=7\nMulti4=42\n" +
        "[IsSpectator]\nMulti1=True\n[SpawnLocations]\nMulti1=90\nMulti2=4\nMulti3=0\nMulti4=7\n" +
        "[Multi2_Alliances]\nHouseAllyOne=2\n[Multi3_Alliances]\nHouseAllyOne=1\n";

    private static ReplayPlayerRow Row(ReplayMatchSummary summary, string name)
    {
        ReplayPlayerRow row = summary.Players.FirstOrDefault(player => player.Name == name);
        if (row == null) throw new Exception("missing player row " + name);
        return row;
    }

    // 对局摘要只读取清单内的 spawn.ini；这里覆盖客户端定义解析、真人/电脑/观察者、颜色与阵营映射、出生点、同盟与不可确认字段。
    private static void DisplaySummaryTests()
    {
        ReplayDisplayCatalog catalog = ReplayDisplayCatalog.Parse(Encoding.UTF8.GetBytes(SampleGameOptions));
        Check(catalog.SideName(4) == "拉丁同盟" && catalog.SideName(11) == "最后堡垒" && catalog.SideName(12) == null &&
            catalog.SideName(-1) == null, "catalog side names");
        ReplayColorDefinition green = catalog.FindColor(3);
        Check(green != null && green.Name == "浅绿色" && green.R == 1 && green.G == 255 && green.B == 0 && catalog.FindColor(99) == null,
            "catalog color mapping");
        string[] optionKeys = catalog.Options.Select(option => option.SpawnKey).ToArray();
        Check(optionKeys.SequenceEqual(new[] { "UnitCount", "GameSpeed", "Credits", "ShortGame", "NavalCombat", "Crates" }),
            "catalog keeps only labelled spawn.ini options in lobby order (MapCode, unlabelled, textless and hidden entries excluded): " +
            string.Join(",", optionKeys));
        ReplayDisplayCatalog internalMap = ReplayDisplayCatalog.Parse(Encoding.UTF8.GetBytes("[General]\nSides=甲,乙,丙\nInternalSideIndices=5,6,7\n"));
        Check(internalMap.SideName(6) == "乙" && internalMap.SideName(0) == null && internalMap.SideName(7) == "丙",
            "internal side indices map back to client sides");
        Check(ReplayDisplayCatalog.Parse(new byte[0]).SideName(0) == null && ReplayDisplayCatalog.Parse(null).Options.Count == 0,
            "empty client definitions");

        ReplayMatchSummary four = ReplayMatchSummary.Parse(Encoding.UTF8.GetBytes(SampleSpawnFourHumans), catalog);
        Check(four.GameMode == "常规作战" && four.ParticipantCount == 4 && four.SpectatorCount == 0 && four.AiCount == 0 &&
            four.Players.Count == 4, "four-human summary counts");
        ReplayPlayerRow recorder = Row(four, "PlayerA");
        Check(recorder.IsRecorder && !recorder.IsAi && recorder.SideText == "拉丁同盟" && recorder.ColorText == "浅绿色" &&
            recorder.HasColor && recorder.ColorR == 1 && recorder.ColorG == 255 && recorder.ColorB == 0 &&
            recorder.StartText == "1" && recorder.AllianceText == "-", "recorder row (Multi1: lowest colour, waypoint 0 shown as start 1)");
        ReplayPlayerRow second = Row(four, "PlayerB");
        Check(!second.IsRecorder && second.SideText == "总部守卫" && second.ColorText == "紫色" && second.StartText == "3",
            "second human row (Multi2: waypoint 2 shown as start 3)");
        Check(Row(four, "PlayerC").StartText == "随机" && Row(four, "PlayerD").StartText == "随机" &&
            Row(four, "PlayerD").SideText == "心灵军团", "unwritten start locations are random");
        Check(four.Players[0].Name == "PlayerA" && four.Players[1].Name == "PlayerB", "recorder first, others in file order");
        Check(four.Settings.Select(s => s.Key + "=" + s.Value).SequenceEqual(new[] { "初始部队数=0", "游戏速度=6 最快", "初始资金=10000" }),
            "dropdown settings use client labels and item texts (Index mode maps the stored index to the item text)");
        Check(four.EnabledSwitches.SequenceEqual(new[] { "快速游戏", "无工具箱" }) && four.DisabledSwitches.SequenceEqual(new[] { "禁止海军单位" }),
            "switch states honour the Reversed flag and skip unknown keys");

        ReplayMatchSummary mixed = ReplayMatchSummary.Parse(Encoding.UTF8.GetBytes(SampleSpawnAiAndAlliance), catalog);
        Check(mixed.ParticipantCount == 1 && mixed.SpectatorCount == 1 && mixed.AiCount == 2 && mixed.Players.Count == 4,
            "mixed summary counts");
        ReplayPlayerRow me = Row(mixed, "Me");
        Check(me.IsRecorder && me.SideText == "欧洲联盟" && me.ColorText == "黄色" && me.StartText == "5" && me.AllianceText == "同盟 1",
            "recorder in alliance with an AI (Multi2)");
        ReplayPlayerRow spectator = Row(mixed, "Spec");
        Check(spectator.IsSpectator && spectator.SideText == "观察者" && spectator.ColorText == "青色" && spectator.StartText == "-" &&
            spectator.AllianceText == "-", "spectator row has no side, start or alliance");
        ReplayPlayerRow hardAi = Row(mixed, "电脑（困难）");
        Check(hardAi.IsAi && hardAi.SideText == "苏俄" && hardAi.ColorText == "橙色" && hardAi.StartText == "1" && hardAi.AllianceText == "同盟 1",
            "hard AI row from HouseHandicaps/HouseCountries/HouseColors (Multi3)");
        ReplayPlayerRow easyAi = Row(mixed, "电脑（简单）");
        Check(easyAi.SideText == "阵营 99" && easyAi.ColorText == "颜色 42" && !easyAi.HasColor && easyAi.StartText == "8" &&
            easyAi.AllianceText == "-", "unmapped side/colour show the raw index without guessing");
        Check(mixed.Settings.Select(s => s.Key + "=" + s.Value).SequenceEqual(new[] { "初始部队数=1", "游戏速度=5 很快", "初始资金=7500" }) &&
            mixed.EnabledSwitches.SequenceEqual(new[] { "禁止海军单位" }) && mixed.DisabledSwitches.SequenceEqual(new[] { "快速游戏", "无工具箱" }),
            "options of the mixed sample");

        // 没有客户端定义：不猜测名称，阵营与颜色显示原始序号，选项一概不显示。
        ReplayMatchSummary bare = ReplayMatchSummary.Parse(Encoding.UTF8.GetBytes(SampleSpawnFourHumans), ReplayDisplayCatalog.Empty);
        Check(Row(bare, "PlayerA").SideText == "阵营 4" && Row(bare, "PlayerA").ColorText == "颜色 3" && !Row(bare, "PlayerA").HasColor &&
            bare.Settings.Count == 0 && bare.EnabledSwitches.Count == 0, "no client definitions: raw indices, no options");

        // 颜色重复（或人数与 PlayerCount 不符）时无法把真人对应到 MultiN：出生点与同盟显示“未知”，不张冠李戴。
        string duplicateColors = SampleSpawnFourHumans.Replace("Color=4\n", "Color=3\n");
        ReplayMatchSummary ambiguous = ReplayMatchSummary.Parse(Encoding.UTF8.GetBytes(duplicateColors), catalog);
        Check(ambiguous.Players.All(p => p.StartText == "未知" && p.AllianceText == "未知"), "duplicate colours make positions unknown");
        string wrongCount = SampleSpawnFourHumans.Replace("PlayerCount=4", "PlayerCount=5");
        Check(ReplayMatchSummary.Parse(Encoding.UTF8.GetBytes(wrongCount), catalog).Players.All(p => p.StartText == "未知"),
            "player count mismatch makes positions unknown");

        // 损坏或无关内容不抛异常。
        byte[] noise = new byte[512];
        new Random(7).NextBytes(noise);
        ReplayMatchSummary garbage = ReplayMatchSummary.Parse(noise, catalog);
        Check(garbage.Players.Count == 1 && garbage.Players[0].Name == "（未命名）" && garbage.GameMode == null, "garbage spawn.ini tolerated");
        Check(ReplayMatchSummary.Parse(null, catalog).Players.Count == 1, "null spawn.ini tolerated");
    }

    // 为展示测试生成含指定 spawn.ini 的清单字节。
    private static byte[] ManifestWith(string root, string name, byte[] spawn)
    {
        string directory = Path.Combine(root, name);
        Directory.CreateDirectory(Path.Combine(directory, "Maps"));
        string map = Path.Combine(directory, "Maps", "x.map");
        File.WriteAllText(map, "map", Encoding.UTF8);
        File.WriteAllBytes(Path.Combine(directory, "spawn.ini"), spawn);
        File.WriteAllBytes(Path.Combine(directory, "spawnmap.ini"), Encoding.UTF8.GetBytes("[Map]\r\n"));
        return ReplayLaunchManifest.Create(directory, map, "(4) 测试地图").Serialize();
    }

    private static string[] LineTexts(ReplayDetails details)
    {
        return details.Lines.Select(line => line.Text).ToArray();
    }

    // 展示数据：完成文件与未完成录像的状态/帧数/版本文本，帧数统计的可靠性规则。
    private static void DetailsAndFrameCountTests(string root)
    {
        ReplayDisplayCatalog catalog = ReplayDisplayCatalog.Parse(Encoding.UTF8.GetBytes(SampleGameOptions));
        byte[] manifest = ManifestWith(root, "details", Encoding.UTF8.GetBytes(SampleSpawnAiAndAlliance));
        string directory = Path.Combine(root, "details-files");
        Directory.CreateDirectory(directory);
        string finished = Path.Combine(directory, "完整.yrrep");
        Archive(finished, manifest, Frames(0, 0, 0, 2));
        ReplayArchive finishedArchive = ReplayArchive.Open(finished);
        Check(finishedArchive.CountFrames() == 1u, "finished archive frame count scans the stream");
        ReplayDetails finishedDetails = ReplayDetails.Load(finishedArchive, catalog);
        string[] lines = LineTexts(finishedDetails);
        Check(lines.Contains("基本") && lines.Contains("对局") && lines.Contains("录像") &&
            finishedDetails.Lines.Where(line => line.IsHeader).Select(line => line.Text).SequenceEqual(new[] { "基本", "对局", "录像" }),
            "details headers");
        Check(lines.Contains("地图：(4) 测试地图") && lines.Contains("状态：完整录像") && lines.Contains("帧数：1") &&
            lines.Any(line => line.StartsWith("录制时间：")) && lines.Contains("游戏模式：测试模式") &&
            lines.Contains("人数：参战 1 人，观察者 1 人，电脑 2 个") &&
            lines.Contains("选项：初始部队数 1，游戏速度 5 很快，初始资金 7500") &&
            lines.Contains("已开启：禁止海军单位") && lines.Contains("已关闭：快速游戏、无工具箱") &&
            lines.Any(line => line.StartsWith("文件：完整.yrrep（") && line.EndsWith("）")) &&
            lines.Contains("帧流版本：2（含玩家输入效果，无编队记录）"), "finished details text: " + string.Join(" | ", lines));
        Check(finishedDetails.Players.Count == 4, "details expose the player rows");
        Check(!lines.Any(line => line.Contains("时长") || line.Contains("mm:ss")), "no duration is shown without a reliable frame rate");

        // 帧流末尾多出不完整记录：完成文件的帧数不可靠，必须不显示数字。
        string trailing = Path.Combine(directory, "尾部多余.yrrep");
        Archive(trailing, manifest, Join(Frames(0, 0, 0, 2), new byte[3]));
        ReplayArchive trailingArchive = ReplayArchive.Open(trailing);
        Check(trailingArchive.CountFrames() == null, "finished archive with an invalid tail has no reliable frame count");
        Check(LineTexts(ReplayDetails.Load(trailingArchive, catalog)).Contains("帧数：无法统计（帧流未通过检查）"), "unreliable count is explained");

        // 未完成录像：状态文案固定，帧数即可恢复帧数，无需扫描。
        byte[] partialStream = Join(Join(Frames(0, 0, 0, 2, 1), Effect()), Frames(1, 0, 0, 2, 0).Skip(32).ToArray());
        string partial = Path.Combine(directory, "未完成.yrrep.partial");
        Partial(partial, manifest, partialStream);
        ReplayArchive partialArchive = ReplayArchive.Open(partial);
        Check(partialArchive.CountFrames() == 2u, "unfinalized frame count is the recovered count");
        string[] partialLines = LineTexts(ReplayDetails.Load(partialArchive, catalog));
        Check(partialLines.Contains("状态：未完成录像：可恢复 2 帧，未做完整性校验") && partialLines.Contains("帧数：2") &&
            partialLines.Any(line => line.StartsWith("文件：未完成.yrrep.partial（")), "unfinalized details text");

        // 帧流版本说明：v1 明确不能播放，v3 含编队记录。
        string legacy = Path.Combine(directory, "旧格式.yrrep");
        Archive(legacy, manifest, Frames(0, 0, 0, 1));
        Check(LineTexts(ReplayDetails.Load(ReplayArchive.Open(legacy), catalog))
            .Contains("帧流版本：1（旧格式，缺少玩家输入效果，无法播放，请重新录制）"), "legacy version is flagged unplayable");
        string grouped = Path.Combine(directory, "编队.yrrep");
        Archive(grouped, manifest, SpeedStream(3, 5, null, null));
        ReplayArchive groupedArchive = ReplayArchive.Open(grouped);
        string[] groupedLines = LineTexts(ReplayDetails.Load(groupedArchive, catalog));
        Check(groupedArchive.HasRecordedGroups && !groupedArchive.HasGameSpeedRecords &&
            groupedLines.Contains("帧流版本：3（含玩家输入效果与编队记录）") && groupedLines.Contains("帧数：2"),
            "v3 details text: " + string.Join(" | ", groupedLines));

        // 帧流版本 4 另含游戏速度记录；帧数统计跳过带速度帧多出的 4 字节。
        string speed = Path.Combine(directory, "游戏速度.yrrep");
        Archive(speed, manifest, SpeedStream(4, 5, 6u, null, 3u));
        ReplayArchive speedArchive = ReplayArchive.Open(speed);
        Check(speedArchive.StreamVersion == 4 && speedArchive.HasPlayerInputEffects && speedArchive.HasRecordedGroups &&
            speedArchive.HasGameSpeedRecords && speedArchive.CountFrames() == 3u, "v4 capability and frame count");
        string[] speedLines = LineTexts(ReplayDetails.Load(speedArchive, catalog));
        Check(speedLines.Contains("帧流版本：4（含玩家输入效果、编队与游戏速度记录）") && speedLines.Contains("帧数：3") &&
            speedLines.Contains("状态：完整录像"), "v4 details text: " + string.Join(" | ", speedLines));
        string onlineSpeed = Path.Combine(directory, "联机带速度.yrrep");
        Archive(onlineSpeed, manifest, SpeedStream(4, 3, null, 1u));
        Check(ReplayArchive.Open(onlineSpeed).CountFrames() == null, "v4 multiplayer frame with a speed makes the count unreliable");
        string speedPartial = Path.Combine(directory, "游戏速度.yrrep.partial");
        Partial(speedPartial, manifest, SpeedStream(4, 0, 2u, null, 7u, null));
        ReplayArchive speedPartialArchive = ReplayArchive.Open(speedPartial);
        string[] speedPartialLines = LineTexts(ReplayDetails.Load(speedPartialArchive, catalog));
        Check(speedPartialArchive.IsUnfinalized && speedPartialArchive.RecoveredFrameCount == 2 &&
            speedPartialLines.Contains("状态：未完成录像：可恢复 2 帧，未做完整性校验") && speedPartialLines.Contains("帧数：2") &&
            speedPartialLines.Contains("帧流版本：4（含玩家输入效果、编队与游戏速度记录）"),
            "v4 unfinalized details stop before the out-of-range speed: " + string.Join(" | ", speedPartialLines));

        // 帧流版本 5：布局与规则同版本 4；单机（帧流头 gameMode 0/5）说明录制时已排除飞机绘制对模拟的影响，
        // 联机与版本 4 文本相同。
        const string v5SinglePlayerText = "帧流版本：5（含玩家输入效果、编队与游戏速度记录，录制时已排除飞机绘制对模拟的影响）";
        const string v5MultiplayerText = "帧流版本：5（含玩家输入效果、编队与游戏速度记录）";
        string v5Skirmish = Path.Combine(directory, "v5遭遇战.yrrep");
        Archive(v5Skirmish, manifest, SpeedStream(5, 5, 6u, null, 3u));
        ReplayArchive v5SkirmishArchive = ReplayArchive.Open(v5Skirmish);
        Check(v5SkirmishArchive.StreamVersion == 5 && v5SkirmishArchive.StreamGameMode == 5u &&
            v5SkirmishArchive.HasPlayerInputEffects && v5SkirmishArchive.HasRecordedGroups &&
            v5SkirmishArchive.HasGameSpeedRecords && v5SkirmishArchive.CountFrames() == 3u, "v5 capability and frame count");
        string[] v5SkirmishLines = LineTexts(ReplayDetails.Load(v5SkirmishArchive, catalog));
        Check(v5SkirmishLines.Contains(v5SinglePlayerText) && v5SkirmishLines.Contains("帧数：3") &&
            v5SkirmishLines.Contains("状态：完整录像"), "v5 skirmish details text: " + string.Join(" | ", v5SkirmishLines));
        string v5Online = Path.Combine(directory, "v5联机.yrrep");
        Archive(v5Online, manifest, SpeedStream(5, 3, null, null));
        ReplayArchive v5OnlineArchive = ReplayArchive.Open(v5Online);
        string[] v5OnlineLines = LineTexts(ReplayDetails.Load(v5OnlineArchive, catalog));
        Check(v5OnlineArchive.StreamGameMode == 3u && v5OnlineLines.Contains(v5MultiplayerText) &&
            v5OnlineLines.Contains("帧数：2") && !v5OnlineLines.Any(line => line.Contains("飞机")),
            "v5 multiplayer details text matches v4: " + string.Join(" | ", v5OnlineLines));
        string v5OnlineSpeed = Path.Combine(directory, "v5联机带速度.yrrep");
        Archive(v5OnlineSpeed, manifest, SpeedStream(5, 3, null, 1u));
        Check(ReplayArchive.Open(v5OnlineSpeed).CountFrames() == null, "v5 multiplayer frame with a speed makes the count unreliable");
        string v5Partial = Path.Combine(directory, "v5战役.yrrep.partial");
        Partial(v5Partial, manifest, SpeedStream(5, 0, 2u, null, 7u, null));
        ReplayArchive v5PartialArchive = ReplayArchive.Open(v5Partial);
        string[] v5PartialLines = LineTexts(ReplayDetails.Load(v5PartialArchive, catalog));
        Check(v5PartialArchive.IsUnfinalized && v5PartialArchive.StreamGameMode == 0u && v5PartialArchive.RecoveredFrameCount == 2 &&
            v5PartialLines.Contains("状态：未完成录像：可恢复 2 帧，未做完整性校验") && v5PartialLines.Contains("帧数：2") &&
            v5PartialLines.Contains(v5SinglePlayerText),
            "v5 unfinalized campaign details stop before the out-of-range speed: " + string.Join(" | ", v5PartialLines));
        string v5OnlinePartial = Path.Combine(directory, "v5联机.yrrep.partial");
        Partial(v5OnlinePartial, manifest, SpeedStream(5, 4, null, null));
        string[] v5OnlinePartialLines = LineTexts(ReplayDetails.Load(ReplayArchive.Open(v5OnlinePartial), catalog));
        Check(v5OnlinePartialLines.Contains(v5MultiplayerText),
            "v5 unfinalized multiplayer details text matches v4: " + string.Join(" | ", v5OnlinePartialLines));
        // 版本 4 的单机录像不显示版本 5 的说明（见上方 v4 用例）；gameMode 不可用时按版本 4 文本显示，不臆测为单机。
        Check(!speedLines.Any(line => line.Contains("飞机")), "v4 skirmish details do not claim the aircraft draw fix");
        typeof(ReplayArchive).GetProperty("StreamGameMode").GetSetMethod(true).Invoke(v5SkirmishArchive, new object[] { null });
        string[] unknownModeLines = LineTexts(ReplayDetails.Compose(v5SkirmishArchive, null, null, "3", 1));
        Check(v5SkirmishArchive.StreamGameMode == null && unknownModeLines.Contains(v5MultiplayerText),
            "v5 details without a game mode fall back to the v4 text: " + string.Join(" | ", unknownModeLines));

        // 无法解析清单摘要时仍给出基本信息。
        ReplayDetails failed = ReplayDetails.Compose(finishedArchive, null, "示例错误", "1", 123);
        Check(LineTexts(failed).Contains("对局信息：无法读取（示例错误）") && LineTexts(failed).Contains("文件：完整.yrrep（123 字节）"),
            "details without summary");
    }

    // 容器 v3 的一个块：可压缩时 method 1（原始 deflate），否则 method 0 原样存放；块头带解压后内容的 CRC-32。
    private static byte[] BlockV3(byte[] raw)
    {
        byte[] packed;
        using (MemoryStream output = new MemoryStream())
        {
            using (DeflateStream deflate = new DeflateStream(output, CompressionLevel.Optimal, true)) deflate.Write(raw, 0, raw.Length);
            packed = output.ToArray();
        }
        bool compressed = packed.Length < raw.Length;
        byte[] payload = compressed ? packed : raw;
        using (MemoryStream block = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(block))
        {
            writer.Write(0x4B4C4252u);
            writer.Write(compressed ? 1u : 0u);
            writer.Write((uint)raw.Length);
            writer.Write((uint)payload.Length);
            writer.Write(ReplayBlocks.Crc32(raw, 0, raw.Length));
            writer.Write(payload);
            return block.ToArray();
        }
    }

    // 容器 v3：清单为一个块，帧流每 blockBytes 逻辑字节切一块，tail 追加在帧流段之后（模拟写到一半的块）。
    // 完成文件填写 streamBytes 与内容哈希；未完成录像 flags=0、streamBytes=0、内容哈希全零。
    private static void ArchiveV3(string path, byte[] manifest, byte[] frameStream, bool unfinished, byte[] gameHash = null,
        int blockBytes = 40, byte[] tail = null)
    {
        byte[] metadata = BlockV3(manifest);
        List<byte> segment = new List<byte>();
        for (int start = 0; start < frameStream.Length; start += blockBytes)
            segment.AddRange(BlockV3(frameStream.Skip(start).Take(blockBytes).ToArray()));
        if (tail != null) segment.AddRange(tail);
        byte[] stream = segment.ToArray();
        using (BinaryWriter writer = new BinaryWriter(File.Create(path)))
        {
            writer.Write(Encoding.ASCII.GetBytes("RAREPL2\0"));
            writer.Write(3u);
            writer.Write(96u);
            writer.Write((uint)metadata.Length);
            writer.Write(unfinished ? 0u : 1u);
            writer.Write(unfinished ? 0ul : (ulong)stream.Length);
            writer.Write(gameHash ?? new byte[32]);
            writer.Write(unfinished ? new byte[32] : Hash(Join(metadata, stream)));
            writer.Write(metadata);
            writer.Write(stream);
        }
    }

    // 改动容器 v3 完成文件帧流段中第 index 块块头的一个字节（offset 为块头内偏移），并按实际字节重算内容哈希，
    // 使错误只出现在块层面。
    private static byte[] CorruptBlockV3(byte[] file, int index, int offset)
    {
        byte[] bytes = (byte[])file.Clone();
        int position = 96 + BitConverter.ToInt32(bytes, 16);
        for (int i = 0; i < index; i++) position += 20 + BitConverter.ToInt32(bytes, position + 12);
        bytes[position + offset] ^= 1;
        Buffer.BlockCopy(Hash(bytes.Skip(96).ToArray()), 0, bytes, 64, 32);
        return bytes;
    }

    // 容器 v3（开局清单与帧流按块压缩）：对局信息、帧数统计与列表沿用同一套规则。
    private static void ContainerV3DetailsTests(string root)
    {
        ReplayDisplayCatalog catalog = ReplayDisplayCatalog.Parse(Encoding.UTF8.GetBytes(SampleGameOptions));
        byte[] manifest = ManifestWith(root, "container-v3", Encoding.UTF8.GetBytes(SampleSpawnAiAndAlliance));
        string directory = Path.Combine(root, "container-v3-files");
        Directory.CreateDirectory(directory);
        byte[] stream = SpeedStream(5, 5, 6u, null, 3u);
        string finished = Path.Combine(directory, "压缩.yrrep");
        ArchiveV3(finished, manifest, stream, false);
        ReplayArchive archive = ReplayArchive.Open(finished);
        archive.ValidatePayload();
        checks++;
        string[] lines = LineTexts(ReplayDetails.Load(archive, catalog));
        Check(archive.ContainerVersion == 3 && archive.StreamVersion == 5 && archive.StreamGameMode == 5u &&
            archive.StreamBytes == new FileInfo(finished).Length - 96 - BitConverter.ToUInt32(File.ReadAllBytes(finished), 16) &&
            archive.CountFrames() == 3u && archive.MatchesCurrentHeader() &&
            lines.Contains("状态：完整录像") && lines.Contains("帧数：3") && lines.Contains("地图：(4) 测试地图"),
            "v3 finished details: " + string.Join(" | ", lines));

        // 帧流段第二块 CRC 不符（内容哈希按实际字节重算）：列表照常打开，帧数不可靠，播放前校验拒绝。
        string broken = Path.Combine(directory, "压缩损坏.yrrep");
        File.WriteAllBytes(broken, CorruptBlockV3(File.ReadAllBytes(finished), 1, 16));
        ReplayArchive brokenArchive = ReplayArchive.Open(broken);
        Check(brokenArchive.CountFrames() == null &&
            LineTexts(ReplayDetails.Load(brokenArchive, catalog)).Contains("帧数：无法统计（帧流未通过检查）"),
            "v3 block CRC mismatch makes the frame count unreliable");
        Reject<InvalidDataException>(() => brokenArchive.ValidatePayload(), "v3 block CRC mismatch rejected before playback");

        // 未完成录像：帧流之后是写到一半的块，在它之前截止。
        string partial = Path.Combine(directory, "压缩.yrrep.partial");
        byte[] nextBlock = BlockV3(new byte[100]);
        byte[] halfBlock = nextBlock.Take(nextBlock.Length - 1).ToArray();
        ArchiveV3(partial, manifest, stream, true, null, 40, halfBlock);
        ReplayArchive partialArchive = ReplayArchive.Open(partial);
        partialArchive.ValidatePayload();
        checks++;
        string[] partialLines = LineTexts(ReplayDetails.Load(partialArchive, catalog));
        Check(partialArchive.ContainerVersion == 3 && partialArchive.IsUnfinalized && partialArchive.RecoveredFrameCount == 3 &&
            partialArchive.StreamBytes == new FileInfo(partial).Length - halfBlock.Length - 96 -
                BitConverter.ToUInt32(File.ReadAllBytes(partial), 16) &&
            partialLines.Contains("状态：未完成录像：可恢复 3 帧，未做完整性校验") && partialLines.Contains("帧数：3"),
            "v3 unfinalized details: " + string.Join(" | ", partialLines));

        // 列表：首块无效的 v3 文件打不开，静默跳过；其余照常列出。
        string unreadable = Path.Combine(directory, "首块损坏.yrrep");
        File.WriteAllBytes(unreadable, CorruptBlockV3(File.ReadAllBytes(finished), 0, 0));
        List<ReplayArchive> listed = ReplayArchive.OpenAll(directory);
        Check(listed.Count == 3 && listed.All(item => item.ContainerVersion == 3) &&
            !listed.Any(item => item.Path == Path.GetFullPath(unreadable)), "v3 replays are listed, an unreadable first block is skipped");
    }

    // 删除规则（纯函数）：只允许回放目录下直接存在的 .yrrep / .yrrep.partial。
    private static void DeletePathRuleTests(string root)
    {
        string replays = Path.Combine(root, "Replays");
        string full;
        string error;
        foreach (string good in new[] { "a.yrrep", "A.YRREP", "b.yrrep.partial", "C.Yrrep.PARTIAL", "中文 名字.yrrep", "x.partial.yrrep", "a.b.yrrep" })
            Check(ReplayFileOperations.TryGetDeletablePath(replays, Path.Combine(replays, good), out full, out error) &&
                full == Path.GetFullPath(Path.Combine(replays, good)), "deletable name " + good);
        Check(ReplayFileOperations.TryGetDeletablePath(replays + "\\", Path.Combine(replays, "a.yrrep"), out full, out error),
            "directory with trailing separator");
        Check(ReplayFileOperations.TryGetDeletablePath(replays, Path.Combine(replays, "sub", "..", "a.yrrep"), out full, out error) &&
            full == Path.Combine(replays, "a.yrrep"), "dot-dot that resolves directly under the directory is normalised");
        Check(ReplayFileOperations.TryGetDeletablePath(replays, replays + "/a.yrrep", out full, out error), "forward slash separator");

        foreach (string bad in new[]
        {
            Path.Combine(replays, "sub", "a.yrrep"),                 // 子目录
            Path.Combine(replays, "sub", "deep", "a.yrrep.partial"), // 更深的子目录
            Path.Combine(replays, "..", "a.yrrep"),                  // 越出目录
            Path.Combine(replays, "..", "Replays2", "a.yrrep"),      // 同前缀的兄弟目录
            Path.Combine(root, "Replays2", "a.yrrep"),
            Path.Combine(root, "a.yrrep"),
            Path.Combine(replays, "a.yrrep.bak"),                    // 其他后缀
            Path.Combine(replays, "a.yrrep.partial.bak"),
            Path.Combine(replays, "a.yrrepx"),
            Path.Combine(replays, "a.yrrep.partialx"),
            Path.Combine(replays, "a.txt"),
            Path.Combine(replays, "a.partial"),
            Path.Combine(replays, "a.yrrep.tmp"),
            Path.Combine(replays, "a"),
            Path.Combine(replays, ".yrrep"),                         // 主名为空
            Path.Combine(replays, ".yrrep.partial"),
            replays + "\\a.yrrep:stream",                            // 备用数据流
            replays + "\\a:b.yrrep",
            replays + "\\*.yrrep",                                   // 通配符
            replays + "\\a?.yrrep",
            replays + "\\a<b.yrrep",
            replays + "\\a\"b.yrrep",
            replays,                                                 // 目录本身
            replays + "\\",
            "a.yrrep",                                               // 相对路径
            "",
            "   ",
            null
        })
        {
            bool accepted = ReplayFileOperations.TryGetDeletablePath(replays, bad, out full, out error);
            Check(!accepted && full == null && !string.IsNullOrEmpty(error), "rejected path: " + (bad ?? "null"));
        }
        Check(!ReplayFileOperations.TryGetDeletablePath(null, Path.Combine(replays, "a.yrrep"), out full, out error) &&
            !ReplayFileOperations.TryGetDeletablePath("", Path.Combine(replays, "a.yrrep"), out full, out error),
            "missing replays directory is rejected");
    }

    // 删除检查（只对临时目录里的测试文件操作，且只调用 CheckDeletable）：每一项检查失败都拒绝并保留文件。
    // 常规测试不调用 Delete，因此不会触碰系统回收站；真实移入回收站的用例见 RecycleBinIntegrationTests。
    private static void DeleteFlowTests(string root)
    {
        byte[] manifest = ManifestWith(root, "delete", Encoding.UTF8.GetBytes(SampleSpawnFourHumans));
        string replays = Path.Combine(root, "DeleteReplays");
        Directory.CreateDirectory(Path.Combine(replays, "sub"));
        byte[] frames = Frames(0, 0, 0, 2);

        string refused = Path.Combine(replays, "refused.yrrep");
        Archive(refused, manifest, frames);
        ReplayArchive refusedArchive = ReplayArchive.Open(refused);
        Check(ReplayFileOperations.CheckDeletable(replays, refusedArchive, true) != null && File.Exists(refused),
            "an active replay session blocks deletion");
        Check(ReplayFileOperations.CheckDeletable(replays, null, false) != null, "no selection is refused");
        Check(ReplayFileOperations.CheckDeletable(Path.Combine(root, "OtherDirectory"), refusedArchive, false) != null && File.Exists(refused),
            "a replay outside the replays directory is refused");

        string nested = Path.Combine(replays, "sub", "nested.yrrep");
        Archive(nested, manifest, frames);
        Check(ReplayFileOperations.CheckDeletable(replays, ReplayArchive.Open(nested), false) != null && File.Exists(nested),
            "a replay in a sub directory is refused");

        string wrongSuffix = Path.Combine(replays, "wrong.yrrep.bak");
        Archive(wrongSuffix, manifest, frames);
        Check(ReplayFileOperations.CheckDeletable(replays, ReplayArchive.Open(wrongSuffix), false) != null && File.Exists(wrongSuffix),
            "a valid replay with another suffix is refused");

        // 列表之后文件被改动：MatchesCurrentHeader 为假，拒绝。
        string changed = Path.Combine(replays, "changed.yrrep");
        Archive(changed, manifest, frames);
        ReplayArchive changedArchive = ReplayArchive.Open(changed);
        Archive(changed, manifest, Join(frames, Frames(1, 0, 0, 2, 0).Skip(32).ToArray()));
        string changedReason = ReplayFileOperations.CheckDeletable(replays, changedArchive, false);
        Check(changedReason != null && changedReason.Contains("已被改动") && File.Exists(changed), "a modified file is refused");

        // 被独占占用（正在录制或播放）：拒绝，文件原样保留。
        string locked = Path.Combine(replays, "locked.yrrep.partial");
        Partial(locked, manifest, Join(Frames(0, 0, 0, 2), new byte[0]));
        ReplayArchive lockedArchive = ReplayArchive.Open(locked);
        using (new FileStream(locked, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
        {
            string lockedReason = ReplayFileOperations.CheckDeletable(replays, lockedArchive, false);
            Check(lockedReason != null && File.Exists(locked), "an exclusively locked file is refused: " + lockedReason);
        }
        using (new FileStream(locked, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            string readingReason = ReplayFileOperations.CheckDeletable(replays, lockedArchive, false);
            Check(readingReason != null && File.Exists(locked), "a file being read by another process is refused: " + readingReason);
        }

        // 文件已消失。
        string vanished = Path.Combine(replays, "vanished.yrrep");
        Archive(vanished, manifest, frames);
        ReplayArchive vanishedArchive = ReplayArchive.Open(vanished);
        File.Delete(vanished);
        Check(ReplayFileOperations.CheckDeletable(replays, vanishedArchive, false) != null, "a vanished file is refused");

        // 全部检查通过：完整的完成文件与未完成录像都放行（只检查，不删除）。
        string intact = Path.Combine(replays, "intact.yrrep");
        string intactPartial = Path.Combine(replays, "intact.yrrep.partial");
        Archive(intact, manifest, frames);
        Partial(intactPartial, manifest, frames);
        Check(ReplayFileOperations.CheckDeletable(replays, ReplayArchive.Open(intact), false) == null, "an intact replay passes every check");
        Check(ReplayFileOperations.CheckDeletable(replays, ReplayArchive.Open(intactPartial), false) == null,
            "an intact unfinalized replay passes every check");
        Check(File.Exists(refused) && File.Exists(nested) && File.Exists(wrongSuffix) && File.Exists(changed) && File.Exists(locked) &&
            File.Exists(intact) && File.Exists(intactPartial), "checking never touches the files");
    }

    // 回收站集成用例：真实调用 SHFileOperationW，会往当前用户的回收站放入临时测试文件，因此默认跳过，
    // 只有测试程序收到参数 --recycle-bin-integration 才运行。
    private static void RecycleBinIntegrationTests(string root)
    {
        byte[] manifest = ManifestWith(root, "recycle", Encoding.UTF8.GetBytes(SampleSpawnFourHumans));
        string replays = Path.Combine(root, "RecycleReplays");
        Directory.CreateDirectory(replays);
        byte[] frames = Frames(0, 0, 0, 2);
        string target = Path.Combine(replays, "to-recycle.yrrep");
        string partialTarget = Path.Combine(replays, "to-recycle.yrrep.partial");
        Archive(target, manifest, frames);
        Partial(partialTarget, manifest, frames);
        Check(ReplayFileOperations.Delete(replays, ReplayArchive.Open(target), true) != null && File.Exists(target),
            "Delete honours an active session and leaves the file");
        string result = ReplayFileOperations.Delete(replays, ReplayArchive.Open(target), false);
        Check(result == null && !File.Exists(target), "finished replay moved to the recycle bin: " + result);
        string partialResult = ReplayFileOperations.Delete(replays, ReplayArchive.Open(partialTarget), false);
        Check(partialResult == null && !File.Exists(partialTarget), "unfinalized replay moved to the recycle bin: " + partialResult);
    }

    // ===== 导入 / 导出 =====
    // 所有用例只在临时目录（root 下）读写：不碰游戏目录、下载目录、桌面和回收站。
    // 压缩包用 Ionic.Zip（与客户端读取所用的库相同）现场生成；Ionic 不会写出的危险名字，
    // 先用等长的 ASCII 占位名写入，再在生成的压缩包字节里（本地头与中央目录各一处）原地改写。

    private static string importRoot;
    private static readonly byte[] ImportGameHash = Enumerable.Repeat((byte)0x5A, 32).ToArray();
    private static readonly DateTime ImportNow = new DateTime(2026, 10, 19, 10, 11, 12);

    private static string FileSha(string path)
    {
        using (SHA256 sha = SHA256.Create())
        using (FileStream stream = File.OpenRead(path))
            return BitConverter.ToString(sha.ComputeHash(stream));
    }

    // 目录下全部条目名（文件与子目录），按序数排序；目录不存在返回空。
    private static string[] Entries(string directory)
    {
        if (!Directory.Exists(directory)) return new string[0];
        return Directory.GetFileSystemEntries(directory).Select(entry => Path.GetFileName(entry))
            .OrderBy(name => name, StringComparer.Ordinal).ToArray();
    }

    private static string CaseDirectory(string name)
    {
        string directory = Path.Combine(importRoot, "case-" + name);
        Directory.CreateDirectory(directory);
        return directory;
    }

    // 清单：tag 写进 spawn.ini，所以 tag 不同则内容不同；tag 长度相同则大小相同。
    private static byte[] ImportManifest(string tag)
    {
        return ManifestWith(importRoot, "manifest-" + tag,
            Encoding.UTF8.GetBytes("[Settings]\nName=" + tag + "\nPlayerCount=1\nSide=1\nColor=1\n"));
    }

    // 有效的 v2 帧流：32 字节帧流头 + frames 条空帧记录；frames 不同则大小不同。
    private static byte[] ValidFrameStream(int frames)
    {
        List<byte> bytes = new List<byte>(Frames(0, 0, 0, 2).Take(32));
        for (uint frame = 0; frame < frames; frame++) bytes.AddRange(Record(frame, 0, 0));
        return bytes.ToArray();
    }

    // 写出一个回放文件：完成文件或未完成录像；legacy 为旧格式（帧流版本 1）。
    private static string MakeReplay(string path, string tag, int frames, bool unfinished = false,
        byte[] gameHash = null, bool legacy = false)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        byte[] manifest = ImportManifest(tag);
        byte[] stream = legacy ? Frames(0, 0, 0, 1) : ValidFrameStream(frames);
        byte[] hash = gameHash ?? ImportGameHash;
        if (unfinished) Partial(path, manifest, stream, hash);
        else Archive(path, manifest, stream, hash);
        return path;
    }

    private static void MakeZip(string path, Action<Ionic.Zip.ZipFile> fill, Action<Ionic.Zip.ZipFile> configure = null)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        using (Ionic.Zip.ZipFile zip = new Ionic.Zip.ZipFile())
        {
            if (configure != null) configure(zip);
            fill(zip);
            zip.Save(path);
        }
    }

    private static string Placeholder(int index, int length)
    {
        return "Q" + index.ToString("D2") + new string('x', length - 3);
    }

    // 把压缩包里的占位名（本地头与中央目录各一处）改写为 replacement；长度必须相同。
    private static void PatchName(string zipPath, string placeholder, byte[] replacement)
    {
        byte[] bytes = File.ReadAllBytes(zipPath);
        byte[] target = Encoding.ASCII.GetBytes(placeholder);
        if (target.Length != replacement.Length) throw new Exception("placeholder length differs");
        int patched = 0;
        for (int i = 0; i + target.Length <= bytes.Length; i++)
        {
            bool match = true;
            for (int j = 0; j < target.Length && match; j++) match = bytes[i + j] == target[j];
            if (!match) continue;
            Buffer.BlockCopy(replacement, 0, bytes, i, replacement.Length);
            patched++;
        }
        if (patched != 2) throw new Exception("expected a local and a central name, patched " + patched);
        File.WriteAllBytes(zipPath, bytes);
    }

    private static ReplayImportOptions ImportOptions(string replays, ReplayImportLimits limits = null, byte[] hash = null,
        bool withZip = true)
    {
        return new ReplayImportOptions
        {
            ReplaysDirectory = replays,
            LocalGameExeHash = hash ?? ImportGameHash,
            Limits = limits,
            OpenZip = withZip ? (Func<string, IReplayZipArchive>)IonicReplayZipArchive.Open : null,
            Now = () => ImportNow
        };
    }

    private static string[] SkippedReasons(ReplayImportResult result)
    {
        return result.Skipped.Select(skip => skip.Reason).ToArray();
    }

    // 每次导入之后都要满足：回放文件夹里没有遗留的 .importing 临时文件。
    private static void NoTemporaryFiles(string replays, string name)
    {
        Check(!Directory.Exists(replays) || Directory.GetFiles(replays, "*.importing").Length == 0, "no temporary file left: " + name);
    }

    // ----- 内容识别 -----

    private static void ImportClassifyTests()
    {
        string dir = CaseDirectory("classify");
        string replay = MakeReplay(Path.Combine(dir, "renamed.jpg"), "k1", 2);
        string partial = MakeReplay(Path.Combine(dir, "plain.bin"), "k2", 2, true);
        string zip = Path.Combine(dir, "looks-like-replay.yrrep");
        MakeZip(zip, z => z.AddEntry("a.txt", new byte[] { 1, 2, 3 }));
        string emptyZip = Path.Combine(dir, "empty.zip");
        MakeZip(emptyZip, z => { });
        Check(ReplayImporter.ClassifyFile(replay) == ReplayContentKind.Replay, "classify: finished replay under a .jpg name");
        Check(ReplayImporter.ClassifyFile(partial) == ReplayContentKind.Replay, "classify: unfinished replay under a .bin name");
        Check(ReplayImporter.ClassifyFile(zip) == ReplayContentKind.Zip, "classify: zip under a .yrrep name");
        Check(ReplayImporter.ClassifyFile(emptyZip) == ReplayContentKind.Zip, "classify: archive without entries is still a zip");

        Func<string, byte[], ReplayContentKind> kindOf = (name, bytes) =>
        {
            string path = Path.Combine(dir, name);
            File.WriteAllBytes(path, bytes);
            return ReplayImporter.ClassifyFile(path);
        };
        Check(kindOf("rar4.dat", new byte[] { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00, 1, 2 }) == ReplayContentKind.Rar, "classify: RAR 4");
        Check(kindOf("rar5.dat", new byte[] { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00 }) == ReplayContentKind.Rar, "classify: RAR 5");
        Check(kindOf("seven.dat", new byte[] { 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0, 4 }) == ReplayContentKind.SevenZip, "classify: 7z");
        Check(kindOf("text.yrrep", Encoding.UTF8.GetBytes("hello, this is not a replay")) == ReplayContentKind.Unknown, "classify: text under a .yrrep name");
        Check(kindOf("empty.yrrep", new byte[0]) == ReplayContentKind.Unknown, "classify: empty file");
        // 截断的头：魔数没有写完整，一律不能识别。
        Check(kindOf("t1", Encoding.ASCII.GetBytes("RAREPL")) == ReplayContentKind.Unknown, "classify: replay magic cut at 6 bytes");
        Check(kindOf("t2", Encoding.ASCII.GetBytes("RAREPL2")) == ReplayContentKind.Unknown, "classify: replay magic without the NUL");
        Check(kindOf("t3", new byte[] { 0x50, 0x4B, 0x03 }) == ReplayContentKind.Unknown, "classify: zip magic cut at 3 bytes");
        Check(kindOf("t4", new byte[] { 0x52, 0x61, 0x72, 0x21, 0x1A }) == ReplayContentKind.Unknown, "classify: rar magic cut at 5 bytes");
        Check(kindOf("t5", new byte[] { 0x37, 0x7A, 0xBC, 0xAF, 0x27 }) == ReplayContentKind.Unknown, "classify: 7z magic cut at 5 bytes");
        Check(kindOf("m1", Encoding.ASCII.GetBytes("RAREPL2\0")) == ReplayContentKind.Replay, "classify: complete replay magic alone");
        Check(kindOf("m2", new byte[] { 0x50, 0x4B, 0x03, 0x04 }) == ReplayContentKind.Zip, "classify: complete zip magic alone");
        // 只是普通文本里碰巧含有魔数文字，开头不是则不算。
        Check(kindOf("n1", Encoding.ASCII.GetBytes(" RAREPL2\0")) == ReplayContentKind.Unknown, "classify: magic must be at offset zero");

        byte[] buffer = Encoding.ASCII.GetBytes("RAREPL2\0");
        Check(ReplayImporter.Classify(null, 0) == ReplayContentKind.Unknown, "classify: null head");
        Check(ReplayImporter.Classify(buffer, 5) == ReplayContentKind.Unknown, "classify: valid bytes beyond the valid length are ignored");
        Check(ReplayImporter.Classify(buffer, 8) == ReplayContentKind.Replay, "classify: exact valid length");
    }

    // ----- 命名 -----

    private static void ImportNamingTests()
    {
        Check(ReplayImporter.StripKnownSuffixes("a.yrrep") == "a" && ReplayImporter.StripKnownSuffixes("a.YRREP.PARTIAL") == "a" &&
            ReplayImporter.StripKnownSuffixes("a.partial") == "a" && ReplayImporter.StripKnownSuffixes("a.yrrep.partial.zip") == "a" &&
            ReplayImporter.StripKnownSuffixes("a.yrrep.yrrep") == "a" && ReplayImporter.StripKnownSuffixes("a.txt") == "a.txt" &&
            ReplayImporter.StripKnownSuffixes(".yrrep") == "" && ReplayImporter.StripKnownSuffixes("a.b.yrrep") == "a.b" &&
            ReplayImporter.StripKnownSuffixes("甲.yrrep.1") == "甲.yrrep.1" && ReplayImporter.StripKnownSuffixes(null) == "",
            "known suffixes are stripped, other extensions are kept");
        Check(ReplayImporter.LastSegment("a/b/c.yrrep") == "c.yrrep" && ReplayImporter.LastSegment("..\\..\\x.yrrep") == "x.yrrep" &&
            ReplayImporter.LastSegment("C:\\x\\y.yrrep") == "y.yrrep" && ReplayImporter.LastSegment("/abs/z.yrrep") == "z.yrrep" &&
            ReplayImporter.LastSegment("dir/") == "" && ReplayImporter.LastSegment("plain") == "plain" && ReplayImporter.LastSegment(null) == "",
            "only the file name part of an entry name is used");

        Check(ReplayImporter.SanitizeFileNamePart("a<b>:\"/\\|?*c", 80) == "a_b" + new string('_', 8) + "c", "illegal characters become underscores");
        Check(ReplayImporter.SanitizeFileNamePart("  name. . ", 80) == "name", "leading and trailing spaces and dots are trimmed");
        Check(ReplayImporter.SanitizeFileNamePart("tab\there", 80) == "tab_here", "control characters are replaced");
        Check(ReplayImporter.SanitizeFileNamePart("???", 80) == "" && ReplayImporter.SanitizeFileNamePart("  . ", 80) == "" &&
            ReplayImporter.SanitizeFileNamePart(null, 80) == "" && ReplayImporter.SanitizeFileNamePart("\uFFFD\uFFFD", 80) == "",
            "names without meaningful content are unusable");
        string longName = new string('甲', 200);
        Check(ReplayImporter.SanitizeFileNamePart(longName, 80) == new string('甲', 80), "long names are limited to 80 characters");
        string surrogate = new string('a', 79) + "\uD83D\uDE00" + "tail";
        string cut = ReplayImporter.SanitizeFileNamePart(surrogate, 80);
        Check(cut == new string('a', 79), "a surrogate pair is never split by the length limit");
        Check(ReplayImporter.SanitizeFileNamePart("ab.", 80) == "ab" && ReplayImporter.SanitizeFileNamePart("ab. c", 80) == "ab. c" &&
            ReplayImporter.SanitizeFileNamePart("a b.c", 80) == "a b.c", "inner spaces and dots survive");
        Check(!ReplayImporter.IsUnreliableName("对局记录.yrrep") && !ReplayImporter.IsUnreliableName("plain.yrrep") && !ReplayImporter.IsUnreliableName("€ 单价") &&
            !ReplayImporter.IsUnreliableName("") && !ReplayImporter.IsUnreliableName(null) &&
            ReplayImporter.IsUnreliableName("a" + (char)0xFFFD + "b") && ReplayImporter.IsUnreliableName("a" + (char)0xF8F5) &&
            ReplayImporter.IsUnreliableName("" + (char)0xE000) && !ReplayImporter.IsUnreliableName("" + (char)0xF900),
            "undecodable names are recognised by replacement and private-use characters");

        Check(ReplayImporter.AvoidReservedDeviceName("CON") == "_CON" && ReplayImporter.AvoidReservedDeviceName("nul.txt") == "_nul.txt" &&
            ReplayImporter.AvoidReservedDeviceName("Com1") == "_Com1" && ReplayImporter.AvoidReservedDeviceName("LPT9") == "_LPT9" &&
            ReplayImporter.AvoidReservedDeviceName("CONSOLE") == "CONSOLE" && ReplayImporter.AvoidReservedDeviceName("COM10") == "COM10" &&
            ReplayImporter.AvoidReservedDeviceName("a.con") == "a.con", "reserved device names are prefixed");

        Check(ReplayImporter.BuildBaseName("我的回放.yrrep", ImportNow, 80) == "我的回放", "base name drops the known suffix");
        Check(ReplayImporter.BuildBaseName("a?b.yrrep.partial", ImportNow, 80) == "a_b", "base name replaces illegal characters");
        Check(ReplayImporter.BuildBaseName("../../x/y.yrrep", ImportNow, 80) == "y", "base name never carries directories");
        Check(ReplayImporter.BuildBaseName(".yrrep", ImportNow, 80) == "导入-20261019-101112" &&
            ReplayImporter.BuildBaseName("", ImportNow, 80) == "导入-20261019-101112" &&
            ReplayImporter.BuildBaseName("???.yrrep", ImportNow, 80) == "导入-20261019-101112" &&
            ReplayImporter.BuildBaseName(null, ImportNow, 80) == "导入-20261019-101112", "unusable names fall back to a time stamp name");
        Check(ReplayImporter.BuildBaseName("CON.yrrep", ImportNow, 80) == "_CON", "reserved device base names are prefixed");
        Check(ReplayImporter.BuildBaseName(new string('长', 300) + ".yrrep", ImportNow, 80).Length == 80, "base name is limited to 80 characters");
        Check(ReplayImporter.BuildBaseName(new string('长', 300) + ".yrrep", ImportNow, 12).Length == 12, "base name honours a smaller limit");

        string shortDirectory = Path.Combine(importRoot, "x");
        string longDirectory = Path.Combine(importRoot, new string('d', 120));
        int shortRoom = ReplayImporter.MaxBaseNameChars(shortDirectory, ".yrrep.partial");
        int longRoom = ReplayImporter.MaxBaseNameChars(longDirectory, ".yrrep.partial");
        Check(shortRoom == Math.Min(80, 259 - shortDirectory.Length - 1 - ".yrrep.partial".Length - 5), "a normal directory allows up to 80 characters");
        Check(longRoom < shortRoom && longRoom >= 8 && (longRoom == 8 || longDirectory.Length + 1 + longRoom + ".yrrep.partial".Length + 5 <= 259),
            "a long directory shrinks the base name so the path still fits");

        // 重名：依次追加 -2、-3，永不覆盖已有文件；同名目录也不会被占用。
        string dir = CaseDirectory("unique");
        File.WriteAllText(Path.Combine(dir, "game.yrrep"), "first");
        File.WriteAllText(Path.Combine(dir, "game-2.yrrep"), "second");
        Directory.CreateDirectory(Path.Combine(dir, "game-3.yrrep"));
        string temp1 = Path.Combine(dir, "t1.importing");
        File.WriteAllText(temp1, "new one");
        string landed = ReplayImporter.MoveToUniquePath(temp1, dir, "game", ".yrrep");
        Check(Path.GetFileName(landed) == "game-4.yrrep" && File.ReadAllText(landed) == "new one" && !File.Exists(temp1) &&
            File.ReadAllText(Path.Combine(dir, "game.yrrep")) == "first" && File.ReadAllText(Path.Combine(dir, "game-2.yrrep")) == "second",
            "duplicate names get -N and never overwrite existing files or directories");
        string temp2 = Path.Combine(dir, "t2.importing");
        File.WriteAllText(temp2, "fresh");
        Check(Path.GetFileName(ReplayImporter.MoveToUniquePath(temp2, dir, "other", ".yrrep.partial")) == "other.yrrep.partial",
            "an unused name is used as is");

        // 同内容检测：先比大小，再比 SHA-256；大小相同内容不同不算重复；被占用的文件跳过。
        string same = CaseDirectory("identical");
        string one = MakeReplay(Path.Combine(same, "one.yrrep"), "AA", 3);
        string two = MakeReplay(Path.Combine(same, "two.yrrep"), "BB", 3);
        string candidate = Path.Combine(importRoot, "candidate.importing");
        File.Copy(two, candidate, true);
        Check(new FileInfo(one).Length == new FileInfo(two).Length && FileSha(one) != FileSha(two), "fixtures: same size, different content");
        Check(ReplayImporter.FindIdenticalReplay(same, candidate) == two, "identical content is found even when a same-size file differs");
        File.Delete(two);
        Check(ReplayImporter.FindIdenticalReplay(same, candidate) == null, "same size but different content is not a duplicate");
        string lockedCopy = Path.Combine(same, "locked.yrrep");
        File.Copy(candidate, lockedCopy);
        using (new FileStream(lockedCopy, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
            Check(ReplayImporter.FindIdenticalReplay(same, candidate) == null, "a file in use cannot be compared and is skipped");
        Check(ReplayImporter.FindIdenticalReplay(same, candidate) == lockedCopy, "the same file is found once it is released");
        File.Delete(candidate);

        // 过期的导入临时文件：只清理“32 位十六进制 + .importing”且足够旧的。
        string stale = CaseDirectory("stale");
        string oldTemp = Path.Combine(stale, new string('a', 32) + ".importing");
        string recentTemp = Path.Combine(stale, new string('B', 32) + ".importing");
        string userFile = Path.Combine(stale, "mine.importing");
        string shortHex = Path.Combine(stale, new string('c', 31) + ".importing");
        string nonHex = Path.Combine(stale, new string('g', 32) + ".importing");
        string replayFile = Path.Combine(stale, "old.yrrep");
        foreach (string path in new[] { oldTemp, recentTemp, userFile, shortHex, nonHex, replayFile }) File.WriteAllText(path, "x");
        DateTime now = new DateTime(2026, 10, 19, 12, 0, 0, DateTimeKind.Utc);
        foreach (string path in new[] { oldTemp, userFile, shortHex, nonHex, replayFile }) File.SetLastWriteTimeUtc(path, now.AddDays(-3));
        File.SetLastWriteTimeUtc(recentTemp, now.AddHours(-2));
        ReplayImporter.CleanStaleTemporaryFiles(stale, TimeSpan.FromDays(1), now);
        Check(!File.Exists(oldTemp) && File.Exists(recentTemp) && File.Exists(userFile) && File.Exists(shortHex) && File.Exists(nonHex) &&
            File.Exists(replayFile), "only old importing temporary files are cleaned");
        ReplayImporter.CleanStaleTemporaryFiles(Path.Combine(stale, "missing"), TimeSpan.FromDays(1), now);
        checks++;
    }

    // ----- 汇总文字 -----

    private static void ImportSummaryTests()
    {
        string nl = Environment.NewLine;
        Check(new ReplayImportResult().BuildSummary(10) == "没有导入任何回放。", "summary of an empty result");
        ReplayImportResult onlyImported = new ReplayImportResult();
        onlyImported.AddImported("a");
        onlyImported.AddImported("b");
        onlyImported.AddImported("c");
        Check(onlyImported.BuildSummary(10) == "成功导入 3 个回放。", "summary with imports only");

        ReplayImportResult nothing = new ReplayImportResult();
        nothing.Skip("a.txt", "这不是回放文件", null);
        Check(nothing.BuildSummary(10) == "没有导入任何回放。" + nl + nl + "原因：" + nl + "- a.txt：这不是回放文件", "summary explains why nothing was imported");

        ReplayImportResult many = new ReplayImportResult();
        many.AddImported("a");
        many.AddImported("b");
        for (int i = 1; i <= 12; i++) many.Skip("文件" + i, "原因" + i, "细节" + i);
        string[] lines = many.BuildSummary(10).Split(new[] { nl }, StringSplitOptions.None);
        Check(lines.Length == 14 && lines[0] == "成功导入 2 个回放。" && lines[1] == "" && lines[2] == "以下 12 项没有导入：" &&
            lines[3] == "- 文件1：原因1" && lines[12] == "- 文件10：原因10" && lines[13] == "等 2 个（未全部列出）",
            "summary lists at most 10 skipped items and counts the rest: " + string.Join("|", lines));
        ReplayImportResult ten = new ReplayImportResult();
        ten.AddImported("a");
        for (int i = 1; i <= 10; i++) ten.Skip("f" + i, "r", null);
        Check(!ten.BuildSummary(10).Contains("等 ") && ten.BuildSummary(10).Split(new[] { nl }, StringSplitOptions.None).Length == 13,
            "ten skipped items are all listed without a remainder line");
        ReplayImportResult trimmed = new ReplayImportResult();
        trimmed.Skip("one", "r", null);
        trimmed.Skip("two", "r", null);
        trimmed.Skip("three", "r", null);
        trimmed.RemoveSkippedFrom(1);
        Check(trimmed.Skipped.Count == 1 && trimmed.Skipped[0].Source == "one", "skipped items can be trimmed back when an archive aborts");
        trimmed.RemoveSkippedFrom(5);
        trimmed.RemoveSkippedFrom(-1);
        Check(trimmed.Skipped.Count == 1, "trimming outside the list does nothing");

        ReplayImportResult longNames = new ReplayImportResult();
        longNames.Skip(new string('长', 100), "原因", null);
        string shortened = longNames.BuildSummary(10).Split(new[] { nl }, StringSplitOptions.None)[3];
        Check(shortened.Length < 60 && shortened.Contains("…") && shortened.EndsWith("：原因"), "long names are shortened in the summary");
        Check(ReplayImportResult.Shorten("short", 40) == "short" && ReplayImportResult.Shorten(new string('x', 40), 40) == new string('x', 40) &&
            ReplayImportResult.Shorten(new string('x', 41), 40).Length == 40, "shortening keeps short names and limits long ones");
    }

    // ----- 校验与落地（独立回放文件）-----

    private static void ImportValidationTests()
    {
        string cd = CaseDirectory("loose");
        string sources = Path.Combine(cd, "Sources");
        string replays = Path.Combine(cd, "Replays"); // 故意不创建：导入应当自己创建
        Check(!Directory.Exists(replays), "fixture: the replays directory does not exist yet");

        string finished = MakeReplay(Path.Combine(sources, "对局 A.yrrep"), "A1", 3);
        string unfinished = MakeReplay(Path.Combine(sources, "对局 B.yrrep.partial"), "B1", 2, true);
        string wrongNameUnfinished = MakeReplay(Path.Combine(sources, "named-wrong.yrrep"), "C1", 2, true);
        string wrongNameFinished = MakeReplay(Path.Combine(sources, "fin.yrrep.partial"), "D1", 2);
        string renamed = MakeReplay(Path.Combine(sources, "renamed.jpg"), "E1", 2);
        Dictionary<string, string> before = Directory.GetFiles(sources).ToDictionary(path => path, path => FileSha(path));

        ReplayImportResult result = ReplayImporter.Import(new[] { finished, unfinished, wrongNameUnfinished, wrongNameFinished, renamed },
            ImportOptions(replays));
        Check(Directory.Exists(replays), "the replays directory is created by the import");
        Check(result.Skipped.Count == 0 && result.ImportedPaths.Count == 5, "five valid replays are imported: " + string.Join("|", SkippedReasons(result)));
        Check(Entries(replays).SequenceEqual(new[] { "fin.yrrep", "named-wrong.yrrep.partial", "renamed.jpg.yrrep", "对局 A.yrrep", "对局 B.yrrep.partial" }
            .OrderBy(name => name, StringComparer.Ordinal)), "suffix follows the container flags, not the original name: " + string.Join("|", Entries(replays)));
        Check(result.FirstImportedPath == Path.GetFullPath(Path.Combine(replays, "对局 A.yrrep")) &&
            result.ImportedPaths.All(path => Path.GetDirectoryName(path) == Path.GetFullPath(replays)), "imported paths are full paths in selection order");
        Check(FileSha(Path.Combine(replays, "对局 A.yrrep")) == before[finished] && FileSha(Path.Combine(replays, "对局 B.yrrep.partial")) == before[unfinished],
            "imported files are byte for byte copies");
        Check(ReplayArchive.Open(Path.Combine(replays, "对局 B.yrrep.partial")).IsUnfinalized &&
            !ReplayArchive.Open(Path.Combine(replays, "fin.yrrep")).IsUnfinalized, "the imported files keep their completion state");
        Check(Directory.GetFiles(sources).All(path => before[path] == FileSha(path)) && Directory.GetFiles(sources).Length == before.Count,
            "source files are untouched");
        NoTemporaryFiles(replays, "valid imports");
        Check(ReplayArchive.OpenAll(replays).Count == 5, "the list can open every imported replay");

        // 再导入一次：内容完全相同，跳过并注明。
        ReplayImportResult again = ReplayImporter.Import(new[] { finished, unfinished }, ImportOptions(replays));
        Check(again.ImportedPaths.Count == 0 && again.Skipped.Count == 2 &&
            again.Skipped.All(skip => skip.Reason.StartsWith("已经导入过了")) && again.Skipped[0].Reason.Contains("对局 A.yrrep") &&
            again.Skipped[0].Source == "对局 A.yrrep" && Entries(replays).Length == 5, "identical files are skipped as already imported");
        string copyOfFinished = Path.Combine(cd, "Sources2", "另一个名字.yrrep");
        Directory.CreateDirectory(Path.GetDirectoryName(copyOfFinished));
        File.Copy(finished, copyOfFinished);
        Check(ReplayImporter.Import(new[] { copyOfFinished }, ImportOptions(replays)).Skipped.Count == 1 && Entries(replays).Length == 5,
            "identical content under another name is also skipped");
        NoTemporaryFiles(replays, "duplicate imports");

        // 同名但内容不同：追加 -2、-3，已有文件不被覆盖。
        string beforeA = FileSha(Path.Combine(replays, "对局 A.yrrep"));
        string collide1 = MakeReplay(Path.Combine(cd, "Sources2", "对局 A.yrrep"), "A2", 4);
        string collide2 = MakeReplay(Path.Combine(cd, "Sources3", "对局 A.yrrep"), "A3", 5);
        ReplayImportResult collisions = ReplayImporter.Import(new[] { collide1, collide2 }, ImportOptions(replays));
        Check(collisions.ImportedPaths.Select(path => Path.GetFileName(path)).SequenceEqual(new[] { "对局 A-2.yrrep", "对局 A-3.yrrep" }) &&
            FileSha(Path.Combine(replays, "对局 A.yrrep")) == beforeA, "colliding names get -2, -3 and never overwrite");

        // 旧格式、游戏版本不符、读不到本机游戏程序。
        string legacy = MakeReplay(Path.Combine(sources, "legacy.yrrep"), "L1", 1, false, null, true);
        string legacyPartial = MakeReplay(Path.Combine(sources, "legacy.yrrep.partial"), "L2", 1, true, null, true);
        string otherGame = MakeReplay(Path.Combine(sources, "other-game.yrrep"), "G1", 2, false, Enumerable.Repeat((byte)0x11, 32).ToArray());
        string otherGamePartial = MakeReplay(Path.Combine(sources, "other-game.yrrep.partial"), "G2", 2, true, Enumerable.Repeat((byte)0x11, 32).ToArray());
        int fileCount = Entries(replays).Length;
        ReplayImportResult rejected = ReplayImporter.Import(new[] { legacy, legacyPartial, otherGame, otherGamePartial }, ImportOptions(replays));
        Check(rejected.ImportedPaths.Count == 0 && rejected.Skipped.Count == 4, "legacy and foreign replays are all rejected");
        Check(rejected.Skipped[0].Reason == "旧格式缺少玩家输入效果，请重新录制" && rejected.Skipped[1].Reason == "旧格式缺少玩家输入效果，请重新录制",
            "frame stream version 1 is rejected with the same wording as the launch");
        Check(rejected.Skipped[2].Reason == "这个回放是用不同版本的游戏录制的，无法在本机播放" &&
            rejected.Skipped[3].Reason == "这个回放是用不同版本的游戏录制的，无法在本机播放", "a different game version is rejected with the specified wording");
        Check(Entries(replays).Length == fileCount, "rejected replays leave nothing behind");
        NoTemporaryFiles(replays, "rejected imports");
        ReplayImportResult unknownGame = ReplayImporter.Import(new[] { finished }, new ReplayImportOptions
            { ReplaysDirectory = replays, LocalGameExeHash = null, OpenZip = IonicReplayZipArchive.Open });
        Check(unknownGame.ImportedPaths.Count == 0 && unknownGame.Skipped[0].Reason == ReplayImporter.GameUnreadableText, "no local game hash: refuse to guess");

        // 截断、损坏、没有可恢复帧：拒绝且不留临时文件。
        string good = MakeReplay(Path.Combine(sources, "good-source.yrrep"), "T1", 6);
        byte[] goodBytes = File.ReadAllBytes(good);
        string truncated = Path.Combine(sources, "truncated.yrrep");
        File.WriteAllBytes(truncated, goodBytes.Take(goodBytes.Length - 7).ToArray());
        string flipped = Path.Combine(sources, "flipped.yrrep");
        byte[] flippedBytes = (byte[])goodBytes.Clone();
        flippedBytes[flippedBytes.Length - 3] ^= 0x40;
        File.WriteAllBytes(flipped, flippedBytes);
        string magicOnly = Path.Combine(sources, "magic-only.yrrep");
        File.WriteAllBytes(magicOnly, Encoding.ASCII.GetBytes("RAREPL2\0"));
        string garbageAfterMagic = Path.Combine(sources, "garbage.yrrep");
        File.WriteAllBytes(garbageAfterMagic, Join(Encoding.ASCII.GetBytes("RAREPL2\0"), new byte[500]));
        string noFrames = MakeReplay(Path.Combine(sources, "no-frames.yrrep.partial"), "T2", 1, true);
        byte[] noFrameBytes = File.ReadAllBytes(noFrames);
        File.WriteAllBytes(noFrames, noFrameBytes.Take(noFrameBytes.Length - 10).ToArray()); // 唯一的帧被截断
        string shortHeader = Path.Combine(sources, "short-header.yrrep");
        File.WriteAllBytes(shortHeader, goodBytes.Take(60).ToArray());
        string[] broken = { truncated, flipped, magicOnly, garbageAfterMagic, noFrames, shortHeader };
        fileCount = Entries(replays).Length;
        ReplayImportResult brokenResult = ReplayImporter.Import(broken, ImportOptions(replays));
        Check(brokenResult.ImportedPaths.Count == 0 && brokenResult.Skipped.Count == broken.Length &&
            brokenResult.Skipped.All(skip => skip.Reason == ReplayImporter.CorruptedReplayText && !string.IsNullOrEmpty(skip.Detail)),
            "truncated or corrupted replays are rejected with a plain reason and a technical detail: " + string.Join("|", SkippedReasons(brokenResult)));
        Check(Entries(replays).Length == fileCount, "corrupted replays leave nothing behind");
        NoTemporaryFiles(replays, "corrupted imports");

        // 不是回放：文本、rar、7z、空文件；以及找不到的文件。
        string text = Path.Combine(sources, "readme.txt");
        File.WriteAllText(text, "hello");
        string rar = Path.Combine(sources, "pack.rar");
        File.WriteAllBytes(rar, new byte[] { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00, 9, 9, 9 });
        string seven = Path.Combine(sources, "pack.7z");
        File.WriteAllBytes(seven, new byte[] { 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0, 4, 9, 9 });
        string empty = Path.Combine(sources, "empty.yrrep");
        File.WriteAllBytes(empty, new byte[0]);
        string missing = Path.Combine(sources, "missing.yrrep");
        ReplayImportResult notReplays = ReplayImporter.Import(new[] { text, rar, seven, empty, missing }, ImportOptions(replays));
        Check(notReplays.ImportedPaths.Count == 0 && notReplays.Skipped.Count == 5, "non-replay files are all skipped");
        Check(notReplays.Skipped[0].Reason == "这不是回放文件" && notReplays.Skipped[3].Reason == "这不是回放文件", "other files are reported as not a replay");
        Check(notReplays.Skipped[1].Reason == "暂不支持这种压缩包，请让对方直接发送回放文件或 .zip 压缩包" &&
            notReplays.Skipped[2].Reason == "暂不支持这种压缩包，请让对方直接发送回放文件或 .zip 压缩包", "rar and 7z get the specified wording");
        Check(notReplays.Skipped[4].Reason.Contains("找不到"), "a missing file is explained");
        Check(notReplays.Skipped.Select(skip => skip.Source).SequenceEqual(new[] { "readme.txt", "pack.rar", "pack.7z", "empty.yrrep", "missing.yrrep" }),
            "skipped sources use plain file names");
        NoTemporaryFiles(replays, "non replay imports");

        // 单个文件超过上限。
        string tooLarge = MakeReplay(Path.Combine(sources, "big.yrrep"), "BIG", 40);
        ReplayImportResult large = ReplayImporter.Import(new[] { tooLarge }, ImportOptions(replays,
            new ReplayImportLimits { MaxEntryBytes = new FileInfo(tooLarge).Length - 1, MaxTotalBytes = long.MaxValue, MaxReplayEntries = 10, MaxScannedEntries = 10 }));
        Check(large.ImportedPaths.Count == 0 && large.Skipped[0].Reason == ReplayImporter.TooLargeText, "an oversize file is refused");
        NoTemporaryFiles(replays, "oversize import");
        ReplayImportResult exactFit = ReplayImporter.Import(new[] { tooLarge }, ImportOptions(replays,
            new ReplayImportLimits { MaxEntryBytes = new FileInfo(tooLarge).Length, MaxTotalBytes = long.MaxValue, MaxReplayEntries = 10, MaxScannedEntries = 10 }));
        Check(exactFit.ImportedPaths.Count == 1, "a file exactly at the limit is accepted");

        // 压缩包选项缺失时压缩包不被读取；回放文件夹无法创建时全部说明原因。
        string pack = Path.Combine(sources, "pack.zip");
        MakeZip(pack, z => z.AddEntry("x.yrrep", File.ReadAllBytes(MakeReplay(Path.Combine(sources, "zipped.yrrep"), "Z1", 2))));
        Check(ReplayImporter.Import(new[] { pack }, ImportOptions(replays, null, null, false)).Skipped[0].Reason == ReplayImporter.UnsupportedArchiveText,
            "without a zip reader archives are reported as unsupported");
        string blocker = Path.Combine(cd, "blocker");
        File.WriteAllText(blocker, "a file where the directory should be");
        ReplayImportResult noFolder = ReplayImporter.Import(new[] { finished }, ImportOptions(blocker));
        Check(noFolder.ImportedPaths.Count == 0 && noFolder.Skipped.Count == 1 && noFolder.Skipped[0].Reason == ReplayImporter.FolderUnavailableText &&
            File.ReadAllText(blocker) == "a file where the directory should be", "an unusable replays folder is explained and nothing is damaged");
        Check(ReplayImporter.Import(new string[0], ImportOptions(replays)).Skipped.Count == 0 && ReplayImporter.Import(null, ImportOptions(replays)).ImportedPaths.Count == 0,
            "nothing selected, nothing happens");

        Check(Directory.GetFiles(sources).All(path => !before.ContainsKey(path) || before[path] == FileSha(path)),
            "source files are still untouched after every scenario");
    }

    // 帧流版本 4 起：带游戏速度的有效回放可以导入；违反游戏速度规则的按损坏拒绝，不留下文件。
    // 版本 5 的布局与规则与版本 4 相同，以同一组用例另跑一轮（独立的用例目录与清单标签）。
    private static void ImportGameSpeedTests(uint version)
    {
        string v = "v" + version;
        string tag = version == 4 ? "GS" : "G" + version;
        string cd = CaseDirectory(version == 4 ? "game-speed" : "game-speed-" + v);
        string sources = Path.Combine(cd, "Sources");
        string replays = Path.Combine(cd, "Replays");
        Directory.CreateDirectory(sources);
        Func<string, string, byte[], bool, string> write = (name, suffix, stream, unfinished) =>
        {
            string path = Path.Combine(sources, name);
            if (unfinished) Partial(path, ImportManifest(tag + suffix), stream, ImportGameHash);
            else Archive(path, ImportManifest(tag + suffix), stream, ImportGameHash);
            return path;
        };
        string skirmish = write("遭遇战.yrrep", "1", SpeedStream(version, 5, 6u, null, 2u), false);
        string campaign = write("战役.yrrep.partial", "2", SpeedStream(version, 0, 4u, null, 9u), true);
        string online = write("联机.yrrep", "3", SpeedStream(version, 3, null, null), false);
        ReplayImportResult accepted = ReplayImporter.Import(new[] { skirmish, campaign, online }, ImportOptions(replays));
        Check(accepted.Skipped.Count == 0 && accepted.ImportedPaths.Count == 3,
            "valid " + v + " replays are imported: " + string.Join("|", SkippedReasons(accepted)));
        Check(ReplayArchive.Open(Path.Combine(replays, "战役.yrrep.partial")).RecoveredFrameCount == 2 &&
            ReplayArchive.OpenAll(replays).Count == 3, "imported " + v + " replays open, the partial up to the invalid speed");
        NoTemporaryFiles(replays, v + " imports");

        string[] invalid =
        {
            write("缺速度.yrrep", "4", SpeedStream(version, 5, null, 6u), false),
            write("联机带速度.yrrep", "5", SpeedStream(version, 1, null, 3u), false),
            write("速度越界.yrrep", "6", SpeedStream(version, 5, 6u, 7u), false),
            write("v3带速度.yrrep", "7", SpeedStream(3, 5, 6u), false),
            write("缺速度.yrrep.partial", "8", SpeedStream(version, 0, null, 6u), true),
            write("联机带速度.yrrep.partial", "9", SpeedStream(version, 4, 1u), true)
        };
        ReplayImportResult rejected = ReplayImporter.Import(invalid, ImportOptions(replays));
        Check(rejected.ImportedPaths.Count == 0 && rejected.Skipped.Count == invalid.Length &&
            rejected.Skipped.All(skip => skip.Reason == ReplayImporter.CorruptedReplayText),
            "game speed rule violations are rejected as corrupted: " + string.Join("|", SkippedReasons(rejected)));
        Check(Entries(replays).Length == 3, "rejected " + v + " replays leave nothing behind");
        NoTemporaryFiles(replays, "rejected " + v + " imports");
        if (version >= 5)
        {
            // 帧流版本 6 未知：按损坏拒绝，不留下文件。
            string[] unknown =
            {
                write("版本6.yrrep", "A", SpeedStream(6, 5, 6u), false),
                write("版本6.yrrep.partial", "B", SpeedStream(6, 3, null, null), true)
            };
            ReplayImportResult unknownResult = ReplayImporter.Import(unknown, ImportOptions(replays));
            Check(unknownResult.ImportedPaths.Count == 0 && unknownResult.Skipped.Count == unknown.Length &&
                unknownResult.Skipped.All(skip => skip.Reason == ReplayImporter.CorruptedReplayText) &&
                Entries(replays).Length == 3,
                "stream version 6 is rejected as corrupted: " + string.Join("|", SkippedReasons(unknownResult)));
            NoTemporaryFiles(replays, "rejected version 6 imports");
        }
    }

    // 容器 v3 的回放按同一规则导入：完成文件与未完成录像按 flags 决定后缀，块损坏按损坏拒绝，不留下文件。
    private static void ImportContainerV3Tests()
    {
        string cd = CaseDirectory("container-v3");
        string sources = Path.Combine(cd, "Sources");
        string replays = Path.Combine(cd, "Replays");
        Directory.CreateDirectory(sources);
        byte[] stream = ValidFrameStream(6);
        string finished = Path.Combine(sources, "压缩.bin");
        ArchiveV3(finished, ImportManifest("V3A"), stream, false, ImportGameHash);
        string partial = Path.Combine(sources, "压缩中止.yrrep");
        ArchiveV3(partial, ImportManifest("V3B"), stream, true, ImportGameHash, 40, new byte[7]);
        string broken = Path.Combine(sources, "压缩损坏.yrrep");
        File.WriteAllBytes(broken, CorruptBlockV3(File.ReadAllBytes(finished), 2, 16));
        ReplayImportResult result = ReplayImporter.Import(new[] { finished, partial, broken }, ImportOptions(replays));
        Check(result.ImportedPaths.Count == 2 && result.Skipped.Count == 1 &&
            result.Skipped[0].Reason == ReplayImporter.CorruptedReplayText && !string.IsNullOrEmpty(result.Skipped[0].Detail),
            "v3 replays are imported and a broken block is rejected as corrupted: " + string.Join("|", SkippedReasons(result)));
        Check(Entries(replays).SequenceEqual(new[] { "压缩.bin.yrrep", "压缩中止.yrrep.partial" }.OrderBy(name => name, StringComparer.Ordinal)),
            "v3 suffix follows the container flags: " + string.Join("|", Entries(replays)));
        ReplayArchive importedPartial = ReplayArchive.Open(Path.Combine(replays, "压缩中止.yrrep.partial"));
        Check(ReplayArchive.Open(Path.Combine(replays, "压缩.bin.yrrep")).ContainerVersion == 3 &&
            importedPartial.ContainerVersion == 3 && importedPartial.RecoveredFrameCount == 6 &&
            FileSha(Path.Combine(replays, "压缩.bin.yrrep")) == FileSha(finished), "imported v3 files are byte for byte copies and open");
        NoTemporaryFiles(replays, "v3 imports");
    }

    // 默认上限必须是约定的数值。
    private static void ImportLimitDefaultTests()
    {
        ReplayImportLimits limits = ReplayImportLimits.Default;
        Check(limits.MaxEntryBytes == 96L + 16L * 1024 * 1024 + 1024L * 1024 * 1024 && limits.MaxTotalBytes == 4L * 1024 * 1024 * 1024 &&
            limits.MaxReplayEntries == 200 && limits.MaxScannedEntries == 10000, "default import limits");
    }

    // ----- 压缩包 -----

    private static void ImportZipTests()
    {
        string cd = CaseDirectory("zip");
        string sources = Path.Combine(cd, "Sources");
        string replays = Path.Combine(cd, "Replays");
        byte[] r1 = File.ReadAllBytes(MakeReplay(Path.Combine(cd, "fixtures", "r1.yrrep"), "Z01", 2));
        byte[] r2 = File.ReadAllBytes(MakeReplay(Path.Combine(cd, "fixtures", "r2.yrrep"), "Z02", 3));
        byte[] r3 = File.ReadAllBytes(MakeReplay(Path.Combine(cd, "fixtures", "r3.yrrep"), "Z03", 4));
        byte[] p1 = File.ReadAllBytes(MakeReplay(Path.Combine(cd, "fixtures", "p1.yrrep"), "Z04", 2, true));
        byte[] junk = Encoding.UTF8.GetBytes("not a replay at all");

        // 任意层级的目录、非回放条目被忽略、目录条目不处理、未完成录像按 flags 命名。
        string nested = Path.Combine(sources, "nested.zip");
        string innerZip = Path.Combine(cd, "fixtures", "inner.zip");
        MakeZip(innerZip, z => z.AddEntry("hidden.yrrep", r3)); // 压缩包里的压缩包不递归处理
        MakeZip(nested, z =>
        {
            z.AddEntry("a/b/c/game1.yrrep", r1);
            z.AddEntry("readme.txt", junk);
            z.AddEntry("x/picture.png", new byte[] { 0x89, 0x50, 0x4E, 0x47, 0, 0, 0, 0, 1, 2, 3 });
            z.AddEntry("top.dat", r2);
            z.AddEntry("deep/er/crashed.yrrep", p1);
            z.AddEntry("inner.zip", File.ReadAllBytes(innerZip));
            z.AddDirectoryByName("emptydir");
        });
        Dictionary<string, string> zipHashBefore = new Dictionary<string, string> { { nested, FileSha(nested) } };
        ReplayImportResult nestedResult = ReplayImporter.Import(new[] { nested }, ImportOptions(replays));
        Check(nestedResult.Skipped.Count == 0 && nestedResult.ImportedPaths.Count == 3, "nested replays are imported, junk is ignored: " + string.Join("|", SkippedReasons(nestedResult)));
        Check(Entries(replays).SequenceEqual(new[] { "crashed.yrrep.partial", "game1.yrrep", "top.dat.yrrep" }.OrderBy(n => n, StringComparer.Ordinal)),
            "files land directly in the replays folder, named by flags: " + string.Join("|", Entries(replays)));
        Check(File.ReadAllBytes(Path.Combine(replays, "game1.yrrep")).SequenceEqual(r1) &&
            File.ReadAllBytes(Path.Combine(replays, "crashed.yrrep.partial")).SequenceEqual(p1), "extracted files are byte for byte the originals");
        Check(FileSha(nested) == zipHashBefore[nested], "the source zip is untouched");
        NoTemporaryFiles(replays, "nested zip");

        // 同一个压缩包再导入：全部已导入过。
        ReplayImportResult nestedAgain = ReplayImporter.Import(new[] { nested }, ImportOptions(replays));
        Check(nestedAgain.ImportedPaths.Count == 0 && nestedAgain.Skipped.Count == 3 &&
            nestedAgain.Skipped.All(skip => skip.Reason.StartsWith("已经导入过了") && skip.Source.StartsWith("nested.zip 中的 ")),
            "zip entries identical to existing replays are skipped");

        // 只有杂物的压缩包、空压缩包、里面是嵌套压缩包。
        string junkOnly = Path.Combine(sources, "junk-only.zip");
        MakeZip(junkOnly, z => { z.AddEntry("a.txt", junk); z.AddEntry("b/c.bin", new byte[100]); });
        string emptyZip = Path.Combine(sources, "empty.zip");
        MakeZip(emptyZip, z => { });
        ReplayImportResult noReplays = ReplayImporter.Import(new[] { junkOnly, emptyZip }, ImportOptions(replays));
        Check(noReplays.ImportedPaths.Count == 0 && noReplays.Skipped.Count == 2 &&
            noReplays.Skipped.All(skip => skip.Reason == ReplayImporter.NoReplayInZipText), "archives without a replay are explained");

        // 路径穿越：只取文件名部分；任何文件都不会出现在回放文件夹之外。
        string[] hostile = { "../../../evil1.yrrep", "..\\..\\evil2.yrrep", "/abs/evil3.yrrep", "C:\\evil4.yrrep", "sub/../../evil5.yrrep" };
        byte[][] hostileReplays = Enumerable.Range(0, hostile.Length)
            .Select(i => File.ReadAllBytes(MakeReplay(Path.Combine(cd, "fixtures", "h" + i + ".yrrep"), "H" + i + "0", 2 + i))).ToArray();
        string traversal = Path.Combine(sources, "traversal.zip");
        MakeZip(traversal, z =>
        {
            for (int i = 0; i < hostile.Length; i++) z.AddEntry(Placeholder(i, hostile[i].Length), hostileReplays[i]);
        });
        for (int i = 0; i < hostile.Length; i++) PatchName(traversal, Placeholder(i, hostile[i].Length), Encoding.ASCII.GetBytes(hostile[i]));
        string hostileFolder = Path.Combine(cd, "Replays-hostile");
        string[] outsideBefore = Entries(cd);
        ReplayImportResult hostileResult = ReplayImporter.Import(new[] { traversal }, ImportOptions(hostileFolder));
        Check(hostileResult.Skipped.Count == 0 && hostileResult.ImportedPaths.Count == 5, "hostile names still import: " + string.Join("|", SkippedReasons(hostileResult)));
        Check(Entries(hostileFolder).SequenceEqual(new[] { "evil1.yrrep", "evil2.yrrep", "evil3.yrrep", "evil4.yrrep", "evil5.yrrep" }),
            "path traversal entries land under their plain file name only: " + string.Join("|", Entries(hostileFolder)));
        string[] outsideAfter = Entries(cd);
        Check(outsideAfter.Except(outsideBefore).SequenceEqual(new[] { "Replays-hostile" }), "nothing was created outside the replays folder: " + string.Join("|", outsideAfter.Except(outsideBefore)));
        string rootParent = Path.GetDirectoryName(importRoot);
        Check(!File.Exists(Path.Combine(rootParent, "evil1.yrrep")) && !File.Exists(Path.Combine(importRoot, "evil1.yrrep")) &&
            !File.Exists(Path.Combine(importRoot, "evil2.yrrep")) && !File.Exists(Path.Combine(rootParent, "evil2.yrrep")) &&
            !File.Exists(Path.Combine(cd, "evil5.yrrep")) && !File.Exists("C:\\evil4.yrrep") && !File.Exists("C:\\abs\\evil3.yrrep"),
            "no traversal target exists");
        Check(hostileResult.ImportedPaths.All(path => Path.GetDirectoryName(path) == Path.GetFullPath(hostileFolder)), "every imported path is inside the replays folder");

        // 条目名：GBK（无 UTF-8 标记）、UTF-8（带标记）、保留字、无法可靠解码。
        Encoding gbk = Encoding.GetEncoding(936);
        string names = Path.Combine(sources, "names.zip");
        MakeZip(names, z =>
        {
            z.AddEntry("目录/对局记录.yrrep", r1);
            z.AddEntry("ZZZZZZ.yrrep", r2);
            z.AddEntry("CON.yrrep", r3);
        }, z => { z.AlternateEncoding = gbk; z.AlternateEncodingUsage = Ionic.Zip.ZipOption.Always; });
        PatchName(names, "ZZZZZZ.yrrep", Join(new byte[] { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, Encoding.ASCII.GetBytes(".yrrep")));
        string namesReplays = Path.Combine(cd, "Replays-names");
        ReplayImportResult namesResult = ReplayImporter.Import(new[] { names }, ImportOptions(namesReplays));
        Check(namesResult.Skipped.Count == 0 && namesResult.ImportedPaths.Count == 3, "names scenario imports: " + string.Join("|", SkippedReasons(namesResult)));
        Check(Entries(namesReplays).SequenceEqual(new[] { "_CON.yrrep", "导入-20261019-101112.yrrep", "对局记录.yrrep" }.OrderBy(n => n, StringComparer.Ordinal)),
            "GBK names decode, undecodable names fall back, device names are prefixed: " + string.Join("|", Entries(namesReplays)));

        string utf8Zip = Path.Combine(sources, "utf8.zip");
        MakeZip(utf8Zip, z => z.AddEntry("战役 回放/第1局.yrrep", r1),
            z => { z.AlternateEncoding = Encoding.UTF8; z.AlternateEncodingUsage = Ionic.Zip.ZipOption.AsNecessary; });
        string utf8Replays = Path.Combine(cd, "Replays-utf8");
        Check(ReplayImporter.Import(new[] { utf8Zip }, ImportOptions(utf8Replays)).ImportedPaths.Count == 1 &&
            Entries(utf8Replays).SequenceEqual(new[] { "第1局.yrrep" }), "UTF-8 flagged names decode: " + string.Join("|", Entries(utf8Replays)));

        // 同名条目（不同目录）与已有同名文件：追加 -N，不覆盖。
        string sameNames = Path.Combine(sources, "same-names.zip");
        MakeZip(sameNames, z => { z.AddEntry("a/game.yrrep", r1); z.AddEntry("b/game.yrrep", r2); z.AddEntry("c/game.yrrep", r3); });
        string sameReplays = Path.Combine(cd, "Replays-same");
        Directory.CreateDirectory(sameReplays);
        File.WriteAllText(Path.Combine(sameReplays, "game.yrrep"), "the user's own file");
        ReplayImportResult sameResult = ReplayImporter.Import(new[] { sameNames }, ImportOptions(sameReplays));
        Check(sameResult.ImportedPaths.Select(path => Path.GetFileName(path)).OrderBy(n => n, StringComparer.Ordinal)
                .SequenceEqual(new[] { "game-2.yrrep", "game-3.yrrep", "game-4.yrrep" }) &&
            File.ReadAllText(Path.Combine(sameReplays, "game.yrrep")) == "the user's own file", "same names get -N and the existing file is preserved");

        // 校验失败的条目被拒，其余条目照常导入。
        string mixed = Path.Combine(sources, "mixed.zip");
        byte[] foreign = File.ReadAllBytes(MakeReplay(Path.Combine(cd, "fixtures", "foreign.yrrep"), "Z05", 2, false, Enumerable.Repeat((byte)0x11, 32).ToArray()));
        byte[] oldFormat = File.ReadAllBytes(MakeReplay(Path.Combine(cd, "fixtures", "old.yrrep"), "Z06", 1, false, null, true));
        byte[] cutReplay = r3.Take(r3.Length - 5).ToArray();
        MakeZip(mixed, z =>
        {
            z.AddEntry("ok.yrrep", r1);
            z.AddEntry("foreign.yrrep", foreign);
            z.AddEntry("old.yrrep", oldFormat);
            z.AddEntry("cut.yrrep", cutReplay);
        });
        string mixedReplays = Path.Combine(cd, "Replays-mixed");
        ReplayImportResult mixedResult = ReplayImporter.Import(new[] { mixed }, ImportOptions(mixedReplays));
        Check(mixedResult.ImportedPaths.Count == 1 && Entries(mixedReplays).SequenceEqual(new[] { "ok.yrrep" }), "only the valid entry is imported");
        Check(SkippedReasons(mixedResult).OrderBy(r => r, StringComparer.Ordinal).SequenceEqual(new[]
        {
            ReplayImporter.CorruptedReplayText, ReplayImporter.LegacyFormatText, ReplayImporter.VersionMismatchText
        }.OrderBy(r => r, StringComparer.Ordinal)) && mixedResult.Skipped.All(skip => skip.Source.StartsWith("mixed.zip 中的 ")),
            "invalid entries are explained per entry: " + string.Join("|", SkippedReasons(mixedResult)));
        NoTemporaryFiles(mixedReplays, "mixed zip");
    }

    // 压缩包上限：单条目、总量、回放条目数、检查条目数；超限即中止整个压缩包，不留任何文件。
    private static void ImportZipLimitTests()
    {
        string cd = CaseDirectory("zip-limits");
        string sources = Path.Combine(cd, "Sources");
        byte[] small = File.ReadAllBytes(MakeReplay(Path.Combine(cd, "fixtures", "s.yrrep"), "S01", 1));
        byte[] medium = File.ReadAllBytes(MakeReplay(Path.Combine(cd, "fixtures", "m.yrrep"), "S02", 3));
        byte[] large = File.ReadAllBytes(MakeReplay(Path.Combine(cd, "fixtures", "l.yrrep"), "S03", 60));
        Check(small.Length < medium.Length && medium.Length < large.Length, "fixtures: three different sizes");

        // 单个条目超限（头部声明的大小已超限）：整个压缩包中止，先前处理过的小条目也不落地。
        string entryLimitZip = Path.Combine(sources, "entry-limit.zip");
        MakeZip(entryLimitZip, z => { z.AddEntry("small.yrrep", small); z.AddEntry("large.yrrep", large); });
        string r1 = Path.Combine(cd, "Replays-entry");
        ReplayImportResult entryResult = ReplayImporter.Import(new[] { entryLimitZip }, ImportOptions(r1,
            new ReplayImportLimits { MaxEntryBytes = medium.Length, MaxTotalBytes = long.MaxValue, MaxReplayEntries = 10, MaxScannedEntries = 10 }));
        Check(entryResult.ImportedPaths.Count == 0 && entryResult.Skipped.Count == 1 && entryResult.Skipped[0].Reason == ReplayImporter.ZipLimitText &&
            entryResult.Skipped[0].Source == "entry-limit.zip", "an oversize entry aborts the whole archive");
        Check(Entries(r1).Length == 0, "an aborted archive leaves nothing behind: " + string.Join("|", Entries(r1)));

        // 总量超限：三个条目各自没超单项上限，累计超过总量上限时中止。
        string totalZip = Path.Combine(sources, "total-limit.zip");
        MakeZip(totalZip, z => { z.AddEntry("a.yrrep", small); z.AddEntry("b.yrrep", medium); z.AddEntry("c.yrrep", large); });
        string r2 = Path.Combine(cd, "Replays-total");
        ReplayImportResult totalResult = ReplayImporter.Import(new[] { totalZip }, ImportOptions(r2,
            new ReplayImportLimits { MaxEntryBytes = large.Length, MaxTotalBytes = small.Length + medium.Length + large.Length - 1, MaxReplayEntries = 10, MaxScannedEntries = 10 }));
        Check(totalResult.ImportedPaths.Count == 0 && totalResult.Skipped.Count == 1 && totalResult.Skipped[0].Reason == ReplayImporter.ZipLimitText &&
            Entries(r2).Length == 0, "exceeding the total limit aborts the archive and cleans up: " + string.Join("|", Entries(r2)));
        NoTemporaryFiles(r2, "total limit");
        ReplayImportResult totalFits = ReplayImporter.Import(new[] { totalZip }, ImportOptions(Path.Combine(cd, "Replays-total-ok"),
            new ReplayImportLimits { MaxEntryBytes = large.Length, MaxTotalBytes = small.Length + medium.Length + large.Length, MaxReplayEntries = 10, MaxScannedEntries = 10 }));
        Check(totalFits.ImportedPaths.Count == 3 && totalFits.Skipped.Count == 0, "exactly at the total limit is accepted");

        // 回放条目数超限。
        string countZip = Path.Combine(sources, "count-limit.zip");
        MakeZip(countZip, z => { z.AddEntry("a.yrrep", small); z.AddEntry("b.yrrep", medium); z.AddEntry("c.yrrep", large); });
        string r3 = Path.Combine(cd, "Replays-count");
        ReplayImportResult countResult = ReplayImporter.Import(new[] { countZip }, ImportOptions(r3,
            new ReplayImportLimits { MaxEntryBytes = large.Length, MaxTotalBytes = long.MaxValue, MaxReplayEntries = 2, MaxScannedEntries = 10 }));
        Check(countResult.ImportedPaths.Count == 0 && countResult.Skipped[0].Reason == ReplayImporter.ZipLimitText && Entries(r3).Length == 0,
            "too many replay entries abort the archive");

        // 检查条目数超限（全是杂物）。
        string scanZip = Path.Combine(sources, "scan-limit.zip");
        MakeZip(scanZip, z => { for (int i = 0; i < 8; i++) z.AddEntry("junk" + i + ".txt", new byte[] { (byte)i }); });
        ReplayImportResult scanResult = ReplayImporter.Import(new[] { scanZip }, ImportOptions(Path.Combine(cd, "Replays-scan"),
            new ReplayImportLimits { MaxEntryBytes = large.Length, MaxTotalBytes = long.MaxValue, MaxReplayEntries = 10, MaxScannedEntries = 5 }));
        Check(scanResult.Skipped.Count == 1 && scanResult.Skipped[0].Reason == ReplayImporter.ZipLimitText, "too many entries to inspect abort the archive");

        // 其他压缩包不受影响：超限只中止出问题的那一个。
        string r4 = Path.Combine(cd, "Replays-next");
        ReplayImportResult twoArchives = ReplayImporter.Import(new[] { entryLimitZip, totalZip }, ImportOptions(r4,
            new ReplayImportLimits { MaxEntryBytes = medium.Length, MaxTotalBytes = long.MaxValue, MaxReplayEntries = 10, MaxScannedEntries = 10 }));
        Check(twoArchives.ImportedPaths.Count == 0 && twoArchives.Skipped.Count == 2, "each archive is judged on its own");

        // 不信任头部声明的大小：条目声明很小，实际数据无穷无尽——必须在读到上限附近就停止，且不留文件。
        string fakeReplays = Path.Combine(cd, "Replays-fake");
        long entryLimit = 256 * 1024;
        EndlessStream lying = null;
        FakeZip lyingZip = new FakeZip();
        lyingZip.List.Add(new FakeZipEntry("lying.yrrep", 10, () => lying = new EndlessStream(Encoding.ASCII.GetBytes("RAREPL2\0"))));
        ReplayImportResult lyingResult = ReplayImporter.Import(new[] { MakeZipPath(sources, "fake-lying.zip") }, new ReplayImportOptions
        {
            ReplaysDirectory = fakeReplays,
            LocalGameExeHash = ImportGameHash,
            Limits = new ReplayImportLimits { MaxEntryBytes = entryLimit, MaxTotalBytes = long.MaxValue, MaxReplayEntries = 5, MaxScannedEntries = 5 },
            OpenZip = path => lyingZip
        });
        Check(lyingResult.ImportedPaths.Count == 0 && lyingResult.Skipped.Count == 1 && lyingResult.Skipped[0].Reason == ReplayImporter.ZipLimitText,
            "a lying header does not defeat the limit");
        Check(lying != null && lying.BytesRead > entryLimit && lying.BytesRead <= entryLimit + 80 * 1024,
            "decompression stops right after the limit: read " + (lying == null ? -1 : lying.BytesRead));
        Check(lyingZip.Disposed && Entries(fakeReplays).Length == 0, "the archive is released and no temporary file remains");

        // 总量上限同样按实际读到的字节计数，而不是看声明。
        List<EndlessStream> streams = new List<EndlessStream>();
        FakeZip totalLying = new FakeZip();
        for (int i = 0; i < 4; i++)
            totalLying.List.Add(new FakeZipEntry("e" + i + ".yrrep", 1, () => { EndlessStream s = new EndlessStream(Encoding.ASCII.GetBytes("RAREPL2\0"), 100 * 1024); streams.Add(s); return s; }));
        ReplayImportResult totalLyingResult = ReplayImporter.Import(new[] { MakeZipPath(sources, "fake-total.zip") }, new ReplayImportOptions
        {
            ReplaysDirectory = fakeReplays,
            LocalGameExeHash = ImportGameHash,
            Limits = new ReplayImportLimits { MaxEntryBytes = 1024 * 1024, MaxTotalBytes = 250 * 1024, MaxReplayEntries = 10, MaxScannedEntries = 10 },
            OpenZip = path => totalLying
        });
        long streamed = streams.Sum(s => s.BytesRead);
        Check(totalLyingResult.Skipped.Count == 1 && totalLyingResult.Skipped[0].Reason == ReplayImporter.ZipLimitText &&
            Entries(fakeReplays).Length == 0, "the total limit counts real bytes");
        Check(streamed <= 4 * 8 + 250 * 1024 + 80 * 1024 + 8 * 2, "reading stops near the total limit: " + streamed);

        // 声明的大小超限：不读取数据（只做一次识别读取）。
        FakeZip declared = new FakeZip();
        FakeZipEntry declaredEntry = new FakeZipEntry("huge.yrrep", long.MaxValue / 2, () => new EndlessStream(Encoding.ASCII.GetBytes("RAREPL2\0")));
        declared.List.Add(declaredEntry);
        ReplayImportResult declaredResult = ReplayImporter.Import(new[] { MakeZipPath(sources, "fake-declared.zip") }, new ReplayImportOptions
        {
            ReplaysDirectory = fakeReplays,
            LocalGameExeHash = ImportGameHash,
            Limits = new ReplayImportLimits { MaxEntryBytes = entryLimit, MaxTotalBytes = long.MaxValue, MaxReplayEntries = 5, MaxScannedEntries = 5 },
            OpenZip = path => declared
        });
        Check(declaredResult.Skipped[0].Reason == ReplayImporter.ZipLimitText && declaredEntry.OpenCount == 1 && Entries(fakeReplays).Length == 0,
            "an oversize declared size is refused without extracting");

        // 读取中途出错：该条目被跳过并说明；其余条目正常导入。
        FakeZip failing = new FakeZip();
        failing.List.Add(new FakeZipEntry("bad.yrrep", small.Length, () => new FailingReadStream(small, 40)));
        failing.List.Add(new FakeZipEntry("good.yrrep", small.Length, () => new MemoryStream(small)));
        ReplayImportResult failingResult = ReplayImporter.Import(new[] { MakeZipPath(sources, "fake-failing.zip") }, new ReplayImportOptions
        {
            ReplaysDirectory = fakeReplays,
            LocalGameExeHash = ImportGameHash,
            OpenZip = path => failing
        });
        Check(failingResult.ImportedPaths.Count == 1 && failingResult.Skipped.Count == 1 &&
            failingResult.Skipped[0].Reason == ReplayImporter.CorruptedEntryText && Entries(fakeReplays).SequenceEqual(new[] { "good.yrrep" }),
            "a read failure skips only that entry: " + string.Join("|", SkippedReasons(failingResult)));
        NoTemporaryFiles(fakeReplays, "fake zips");

        // 识别阶段就读不出来的条目：不当作回放；全部读不出来时给出针对性的提示。
        FakeZip unreadable = new FakeZip();
        unreadable.List.Add(new FakeZipEntry("exotic.yrrep", 10, () => { throw new NotSupportedException("unsupported compression"); }));
        ReplayImportResult unreadableResult = ReplayImporter.Import(new[] { MakeZipPath(sources, "fake-unreadable.zip") }, new ReplayImportOptions
        {
            ReplaysDirectory = fakeReplays,
            LocalGameExeHash = ImportGameHash,
            OpenZip = path => unreadable
        });
        Check(unreadableResult.Skipped.Count == 1 && unreadableResult.Skipped[0].Reason.Contains("读取失败") && unreadableResult.ImportedPaths.Count == 0,
            "unreadable entries are reported instead of silently ignored");
    }

    // 为假压缩包准备一个以 PK 开头的占位文件（导入按内容识别后才交给 OpenZip）。
    private static string MakeZipPath(string directory, string name)
    {
        Directory.CreateDirectory(directory);
        string path = Path.Combine(directory, name);
        File.WriteAllBytes(path, new byte[] { 0x50, 0x4B, 0x03, 0x04, 0, 0, 0, 0 });
        return path;
    }

    private sealed class FakeZipEntry : IReplayZipEntry
    {
        private readonly Func<Stream> open;

        public FakeZipEntry(string name, long declared, Func<Stream> open, bool encrypted = false)
        {
            Name = name;
            DeclaredBytes = declared;
            IsEncrypted = encrypted;
            this.open = open;
        }

        public string Name { get; private set; }
        public bool IsDirectory { get { return false; } }
        public bool IsEncrypted { get; private set; }
        public long DeclaredBytes { get; private set; }
        public int OpenCount;

        public Stream Open()
        {
            OpenCount++;
            return open();
        }
    }

    private sealed class FakeZip : IReplayZipArchive
    {
        public readonly List<IReplayZipEntry> List = new List<IReplayZipEntry>();
        public bool Disposed;

        public IList<IReplayZipEntry> Entries { get { return List; } }

        public void Dispose() { Disposed = true; }
    }

    // 以 prefix 开头、其后全是零的“无穷”数据流，记录实际被读走的字节数；safetyLimit 之后返回 0，防止测试卡死。
    private sealed class EndlessStream : Stream
    {
        private readonly byte[] prefix;
        private readonly long safetyLimit;
        public long BytesRead;

        public EndlessStream(byte[] prefix, long safetyLimit = 64L * 1024 * 1024)
        {
            this.prefix = prefix;
            this.safetyLimit = safetyLimit;
        }

        public override int Read(byte[] buffer, int offset, int count)
        {
            int remaining = (int)Math.Min(count, safetyLimit - BytesRead);
            if (remaining <= 0) return 0;
            for (int i = 0; i < remaining; i++)
            {
                long position = BytesRead + i;
                buffer[offset + i] = position < prefix.Length ? prefix[position] : (byte)0;
            }
            BytesRead += remaining;
            return remaining;
        }

        public override bool CanRead { get { return true; } }
        public override bool CanSeek { get { return false; } }
        public override bool CanWrite { get { return false; } }
        public override long Length { get { throw new NotSupportedException(); } }
        public override long Position { get { throw new NotSupportedException(); } set { throw new NotSupportedException(); } }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
        public override void SetLength(long value) { throw new NotSupportedException(); }
        public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
    }

    // 读到 failAt 字节后抛 IOException 的数据流（模拟压缩数据损坏）。
    private sealed class FailingReadStream : Stream
    {
        private readonly byte[] data;
        private readonly int failAt;
        private int position;

        public FailingReadStream(byte[] data, int failAt)
        {
            this.data = data;
            this.failAt = failAt;
        }

        public override int Read(byte[] buffer, int offset, int count)
        {
            if (position >= failAt) throw new InvalidDataException("simulated corrupted data");
            int read = Math.Min(Math.Min(count, data.Length - position), failAt - position);
            Buffer.BlockCopy(data, position, buffer, offset, read);
            position += read;
            return read;
        }

        public override bool CanRead { get { return true; } }
        public override bool CanSeek { get { return false; } }
        public override bool CanWrite { get { return false; } }
        public override long Length { get { throw new NotSupportedException(); } }
        public override long Position { get { throw new NotSupportedException(); } set { throw new NotSupportedException(); } }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
        public override void SetLength(long value) { throw new NotSupportedException(); }
        public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
    }

    // 加密、损坏的压缩包。
    private static void ImportBrokenZipTests()
    {
        string cd = CaseDirectory("zip-broken");
        string sources = Path.Combine(cd, "Sources");
        byte[] r1 = File.ReadAllBytes(MakeReplay(Path.Combine(cd, "fixtures", "r1.yrrep"), "X01", 2));
        byte[] r2 = File.ReadAllBytes(MakeReplay(Path.Combine(cd, "fixtures", "r2.yrrep"), "X02", 3));
        string replays = Path.Combine(cd, "Replays");

        // 加密：整个压缩包被拒绝并说明；混合了未加密条目时同样整体拒绝，避免漏掉看不见内容的加密条目。
        string encrypted = Path.Combine(sources, "secret.zip");
        MakeZip(encrypted, z =>
        {
            Ionic.Zip.ZipEntry entry = z.AddEntry("locked.yrrep", r1);
            entry.Password = "secret";
            entry.Encryption = Ionic.Zip.EncryptionAlgorithm.PkzipWeak;
        });
        string aes = Path.Combine(sources, "aes.zip");
        MakeZip(aes, z =>
        {
            Ionic.Zip.ZipEntry entry = z.AddEntry("locked.yrrep", r1);
            entry.Password = "secret";
            entry.Encryption = Ionic.Zip.EncryptionAlgorithm.WinZipAes256;
        });
        string partlyEncrypted = Path.Combine(sources, "partly.zip");
        MakeZip(partlyEncrypted, z =>
        {
            z.AddEntry("plain.yrrep", r2);
            Ionic.Zip.ZipEntry entry = z.AddEntry("locked.yrrep", r1);
            entry.Password = "secret";
            entry.Encryption = Ionic.Zip.EncryptionAlgorithm.PkzipWeak;
        });
        ReplayImportResult encryptedResult = ReplayImporter.Import(new[] { encrypted, aes, partlyEncrypted }, ImportOptions(replays));
        Check(encryptedResult.ImportedPaths.Count == 0 && encryptedResult.Skipped.Count == 3 &&
            encryptedResult.Skipped.All(skip => skip.Reason == ReplayImporter.EncryptedZipText && skip.Reason.Contains("密码")),
            "password protected archives are refused with a plain reason: " + string.Join("|", SkippedReasons(encryptedResult)));
        Check(Entries(replays).Length == 0, "nothing is extracted from an encrypted archive");

        // 损坏：被截断、只有 PK 头、内容被改动。
        string good = Path.Combine(sources, "good.zip");
        MakeZip(good, z => { z.AddEntry("a.yrrep", r1); z.AddEntry("b.yrrep", r2); });
        byte[] goodBytes = File.ReadAllBytes(good);
        string truncated = Path.Combine(sources, "truncated.zip");
        File.WriteAllBytes(truncated, goodBytes.Take(goodBytes.Length / 2).ToArray());
        string garbage = Path.Combine(sources, "garbage.zip");
        byte[] noise = new byte[400];
        new Random(5).NextBytes(noise);
        File.WriteAllBytes(garbage, Join(new byte[] { 0x50, 0x4B, 0x03, 0x04 }, noise));
        ReplayImportResult brokenResult = ReplayImporter.Import(new[] { truncated, garbage }, ImportOptions(replays));
        Check(brokenResult.ImportedPaths.Count == 0 && brokenResult.Skipped.Count == 2 &&
            brokenResult.Skipped.All(skip => skip.Reason == ReplayImporter.CorruptedZipText && !string.IsNullOrEmpty(skip.Detail)),
            "broken archives are refused with a plain reason: " + string.Join("|", SkippedReasons(brokenResult)));
        Check(Entries(replays).Length == 0, "broken archives leave nothing behind: " + string.Join("|", Entries(replays)));
        NoTemporaryFiles(replays, "broken zips");

        // 只丢了压缩包末尾的目录信息、条目数据完好：Ionic 能顺序读出条目，导入结果必须与原文件逐字节一致（由 CRC 与回放校验保证）；
        // 若读不出来则按损坏处理。无论哪种，都不能导入任何不一致的内容，也不留临时文件。
        string noDirectory = Path.Combine(sources, "no-directory.zip");
        File.WriteAllBytes(noDirectory, goodBytes.Take(goodBytes.Length - 100).ToArray());
        string noDirectoryReplays = Path.Combine(cd, "Replays-no-directory");
        ReplayImportResult noDirectoryResult = ReplayImporter.Import(new[] { noDirectory }, ImportOptions(noDirectoryReplays));
        Check(noDirectoryResult.ImportedPaths.All(path => File.ReadAllBytes(path).SequenceEqual(r1) || File.ReadAllBytes(path).SequenceEqual(r2)) &&
            noDirectoryResult.ImportedPaths.Count + noDirectoryResult.Skipped.Count >= 1, "an archive without its directory imports only intact content");
        NoTemporaryFiles(noDirectoryReplays, "archive without directory");

        // 条目数据被改动（存储方式，结构完好）：CRC 不符，该条目被跳过，其余照常导入。
        string stored = Path.Combine(sources, "stored.zip");
        MakeZip(stored, z => { z.AddEntry("a.yrrep", r1); z.AddEntry("b.yrrep", r2); }, z => z.CompressionLevel = Ionic.Zlib.CompressionLevel.None);
        byte[] storedBytes = File.ReadAllBytes(stored);
        int at = IndexOf(storedBytes, r1) + 120;
        storedBytes[at] ^= 0x10; // 破坏 a.yrrep 的数据
        string flipped = Path.Combine(sources, "flipped.zip");
        File.WriteAllBytes(flipped, storedBytes);
        string flippedReplays = Path.Combine(cd, "Replays-flipped");
        ReplayImportResult flippedResult = ReplayImporter.Import(new[] { flipped }, ImportOptions(flippedReplays));
        Check(flippedResult.ImportedPaths.Count == 1 && Entries(flippedReplays).SequenceEqual(new[] { "b.yrrep" }) &&
            flippedResult.Skipped.Count == 1 && flippedResult.Skipped[0].Reason == ReplayImporter.CorruptedEntryText &&
            flippedResult.Skipped[0].Source == "flipped.zip 中的 a.yrrep", "a corrupted entry is skipped by CRC while the others import: " +
            string.Join("|", SkippedReasons(flippedResult)));
        NoTemporaryFiles(flippedReplays, "flipped zip");
        Check(ReplayImporter.Import(new[] { stored }, ImportOptions(Path.Combine(cd, "Replays-stored"))).ImportedPaths.Count == 2,
            "the unmodified stored archive imports both entries");
    }

    private static int IndexOf(byte[] haystack, byte[] needle)
    {
        for (int i = 0; i + needle.Length <= haystack.Length; i++)
        {
            bool match = true;
            for (int j = 0; j < needle.Length && match; j++) match = haystack[i + j] == needle[j];
            if (match) return i;
        }
        throw new Exception("needle not found");
    }

    // 对话框初始位置的选择规则（不弹出任何窗口）。
    private static void DialogLocationTests(string root)
    {
        string existing = Path.Combine(root, "locations-downloads");
        string desktop = Path.Combine(root, "locations-desktop");
        Directory.CreateDirectory(existing);
        Directory.CreateDirectory(desktop);
        Check(ReplayDialogLocations.ImportStartDirectory(() => existing, () => desktop) == existing, "downloads is preferred");
        Check(ReplayDialogLocations.ImportStartDirectory(() => Path.Combine(root, "no-such-downloads"), () => desktop) == desktop, "missing downloads falls back to the desktop");
        Check(ReplayDialogLocations.ImportStartDirectory(() => null, () => desktop) == desktop, "no downloads folder falls back to the desktop");
        Check(ReplayDialogLocations.ImportStartDirectory(() => { throw new InvalidOperationException(); }, () => desktop) == desktop, "a failing lookup falls back to the desktop");
        Check(ReplayDialogLocations.ImportStartDirectory(() => null, () => Path.Combine(root, "no-such-desktop")) == null, "no usable folder: the dialog default applies");
        string downloads = ReplayDialogLocations.GetDownloadsDirectory();
        Check(downloads == null || Path.IsPathRooted(downloads), "the Downloads known folder lookup works on this machine or reports nothing");
        string realDesktop = ReplayDialogLocations.GetDesktopDirectory();
        Check(realDesktop != null, "the desktop lookup does not fail");
        string start = ReplayDialogLocations.ImportStartDirectory();
        Check(start == null || Directory.Exists(start), "the real import start folder exists when present");
        string exportStart = ReplayDialogLocations.ExportStartDirectory();
        Check(exportStart == null || Directory.Exists(exportStart), "the real export start folder exists when present");
    }

    // ----- 导出 -----

    // 写入时在 limit 字节后抛 IOException（磁盘已满）的包装。
    private sealed class FailingWriteStream : Stream
    {
        private readonly Stream inner;
        private readonly long limit;
        private long written;

        public FailingWriteStream(Stream inner, long limit)
        {
            this.inner = inner;
            this.limit = limit;
        }

        public override void Write(byte[] buffer, int offset, int count)
        {
            if (written + count > limit)
            {
                int allowed = (int)Math.Max(0, limit - written);
                if (allowed > 0) inner.Write(buffer, offset, allowed);
                written += allowed;
                throw new IOException("There is not enough space on the disk.", unchecked((int)0x80070070));
            }
            inner.Write(buffer, offset, count);
            written += count;
        }

        public override void Flush() { inner.Flush(); }
        public override bool CanRead { get { return false; } }
        public override bool CanSeek { get { return false; } }
        public override bool CanWrite { get { return true; } }
        public override long Length { get { throw new NotSupportedException(); } }
        public override long Position { get { throw new NotSupportedException(); } set { throw new NotSupportedException(); } }
        public override int Read(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
        public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
        public override void SetLength(long value) { throw new NotSupportedException(); }
    }

    // 读取时把第一个字节翻转一位（模拟写入后核对时发现内容不一致）。
    private sealed class FlippingReadStream : Stream
    {
        private readonly Stream inner;
        private bool flipped;

        public FlippingReadStream(Stream inner) { this.inner = inner; }

        public override int Read(byte[] buffer, int offset, int count)
        {
            int read = inner.Read(buffer, offset, count);
            if (read > 0 && !flipped)
            {
                buffer[offset] ^= 1;
                flipped = true;
            }
            return read;
        }

        public override bool CanRead { get { return true; } }
        public override bool CanSeek { get { return false; } }
        public override bool CanWrite { get { return false; } }
        public override long Length { get { throw new NotSupportedException(); } }
        public override long Position { get { throw new NotSupportedException(); } set { throw new NotSupportedException(); } }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
        public override void SetLength(long value) { throw new NotSupportedException(); }
        public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
    }

    private static void ExportNameTests()
    {
        DateTime time = new DateTime(2026, 10, 19, 14, 5, 59);
        Check(ReplayExporter.DefaultFileName("(4) 西伯利亚 · 木", time, false) == "(4) 西伯利亚 · 木 2026-10-19 14-05.yrrep", "default export name");
        Check(ReplayExporter.DefaultFileName("(4) 西伯利亚 · 木", time, true) == "(4) 西伯利亚 · 木 2026-10-19 14-05（未完成）.yrrep",
            "an unfinished replay is marked before the extension");
        Check(ReplayExporter.DefaultFileName("a<b>:\"/\\|?*c", time, false) == "a_b" + new string('_', 8) + "c 2026-10-19 14-05.yrrep",
            "illegal characters in the map name are replaced");
        string longName = ReplayExporter.DefaultFileName(new string('图', 300), time, true);
        Check(longName == new string('图', 60) + " 2026-10-19 14-05（未完成）.yrrep" && longName.Length <= 100, "a long map name is limited");
        Check(ReplayExporter.DefaultFileName("", time, false) == "回放 2026-10-19 14-05.yrrep" &&
            ReplayExporter.DefaultFileName("???", time, false) == "回放 2026-10-19 14-05.yrrep" &&
            ReplayExporter.DefaultFileName(null, time, true) == "回放 2026-10-19 14-05（未完成）.yrrep", "an unusable map name falls back to 回放");
        Check(ReplayExporter.DefaultFileName("CON", time, false) == "CON 2026-10-19 14-05.yrrep" &&
            ReplayExporter.DefaultFileName("a.b. ", time, false) == "a.b 2026-10-19 14-05.yrrep", "export names stay valid file names");
        Check(ReplayExporter.DefaultFileName("x", time, false).IndexOfAny(Path.GetInvalidFileNameChars()) < 0, "export names never contain illegal characters");
        Check(ReplayExporter.ExplorerSelectArguments("C:\\Users\\a b\\桌面\\x, y.yrrep") == "/select,\"C:\\Users\\a b\\桌面\\x, y.yrrep\"",
            "explorer arguments quote the whole path");
    }

    private static void ExportDestinationTests(string root)
    {
        string dir = CaseDirectory("export-destination");
        string replays = Path.Combine(dir, "Replays");
        string source = Path.Combine(replays, "src.yrrep");
        string full;
        Check(ReplayExporter.CheckDestination(replays, source, Path.Combine(dir, "Desktop", "ok.yrrep"), out full) == null &&
            full == Path.Combine(dir, "Desktop", "ok.yrrep"), "an ordinary destination is accepted");
        Check(ReplayExporter.CheckDestination(replays, source, Path.Combine(dir, "ok.yrrep"), out full) == null, "the parent of the replays folder is allowed");
        Check(ReplayExporter.CheckDestination(replays, source, Path.Combine(dir, "Replays2", "ok.yrrep"), out full) == null, "a sibling with the same prefix is allowed");
        Check(ReplayExporter.CheckDestination(replays, source, Path.Combine(replays, "..", "ok.yrrep"), out full) == null && full == Path.Combine(dir, "ok.yrrep"),
            "a relative path that leaves the replays folder is allowed");
        Check(ReplayExporter.CheckDestination(replays + "\\", source, Path.Combine(dir, "ok.yrrep"), out full) == null, "a trailing separator on the replays folder is fine");
        Check(ReplayExporter.CheckDestination(replays, source, source, out full) == ReplayExporter.SameAsSourceText && full == null, "the source itself is refused");
        Check(ReplayExporter.CheckDestination(replays, source, source.ToUpperInvariant(), out full) == ReplayExporter.SameAsSourceText, "the source in another case is refused");
        Check(ReplayExporter.CheckDestination(replays, source, replays + "/src.yrrep", out full) == ReplayExporter.SameAsSourceText, "the source with forward slashes is refused");
        Check(ReplayExporter.CheckDestination(replays, source, Path.Combine(replays, "other.yrrep"), out full) == "不能导出到回放文件夹里，请选择桌面或其他位置" &&
            full == null, "a file in the replays folder is refused with the specified wording");
        Check(ReplayExporter.CheckDestination(replays, source, Path.Combine(replays, "Sub", "deep", "other.yrrep"), out full) == ReplayExporter.InsideReplaysText,
            "a file in a sub folder of the replays folder is refused");
        Check(ReplayExporter.CheckDestination(replays, source, Path.Combine(replays.ToUpperInvariant(), "x.yrrep"), out full) == ReplayExporter.InsideReplaysText,
            "the replays folder is matched case-insensitively");
        Check(ReplayExporter.CheckDestination(replays, source, Path.Combine(replays, "a", "..", "x.yrrep"), out full) == ReplayExporter.InsideReplaysText,
            "a dot-dot that stays inside is still inside");
        Check(ReplayExporter.CheckDestination(replays, source, null, out full) != null && ReplayExporter.CheckDestination(replays, source, "  ", out full) != null &&
            ReplayExporter.CheckDestination(replays, source, "", out full) != null, "an empty destination is refused");
        Check(ReplayExporter.CheckDestination(replays, source, Path.Combine(dir, "bad?.yrrep"), out full) != null &&
            ReplayExporter.CheckDestination(replays, source, Path.Combine(dir, "bad*.yrrep"), out full) != null, "illegal file names are refused");
        Check(ReplayExporter.CheckDestination(replays, source, Path.Combine(dir, "folder") + "\\", out full) != null, "a folder path without a file name is refused");
        Check(ReplayExporter.CheckDestination(null, source, Path.Combine(dir, "x.yrrep"), out full) != null &&
            ReplayExporter.CheckDestination("", source, Path.Combine(dir, "x.yrrep"), out full) != null, "an unknown replays folder is refused");
    }

    private static void ExportTests(string root)
    {
        string dir = CaseDirectory("export");
        string replays = Path.Combine(dir, "Replays");
        string outDir = Path.Combine(dir, "Out");
        Directory.CreateDirectory(outDir);
        string finishedPath = MakeReplay(Path.Combine(replays, "src.yrrep"), "EX1", 5);
        string partialPath = MakeReplay(Path.Combine(replays, "crashed.yrrep.partial"), "EX2", 4, true);
        ReplayArchive finished = ReplayArchive.Open(finishedPath);
        ReplayArchive partial = ReplayArchive.Open(partialPath);
        string finishedHash = FileSha(finishedPath);
        string partialHash = FileSha(partialPath);
        string detail;

        // 逐字节一致；源文件不变；目标目录里没有临时文件。
        string target = Path.Combine(outDir, "导出 完成.yrrep");
        Check(ReplayExporter.Export(replays, finished, target, out detail) == null && detail == null &&
            File.ReadAllBytes(target).SequenceEqual(File.ReadAllBytes(finishedPath)), "a finished replay is exported byte for byte");
        string partialTarget = Path.Combine(outDir, "导出 未完成.yrrep");
        Check(ReplayExporter.Export(replays, partial, partialTarget, out detail) == null &&
            File.ReadAllBytes(partialTarget).SequenceEqual(File.ReadAllBytes(partialPath)), "an unfinished replay is exported unchanged, header included");
        Check(ReplayArchive.Open(partialTarget).IsUnfinalized && ReplayArchive.Open(target).MatchesCurrentHeader(), "the exported files open as the same replays");
        Check(FileSha(finishedPath) == finishedHash && FileSha(partialPath) == partialHash, "source files are untouched by exports");
        Check(Directory.GetFiles(outDir, "*.exporting").Length == 0 && Entries(replays).Length == 2, "no temporary file remains and the replays folder is unchanged");

        // 导入器能再读回导出的文件（导出的 .yrrep 是未完成录像时按内容仍被识别为未完成）。
        string roundTrip = Path.Combine(dir, "RoundTrip");
        ReplayImportResult imported = ReplayImporter.Import(new[] { target, partialTarget }, ImportOptions(roundTrip));
        Check(imported.ImportedPaths.Count == 2 && Entries(roundTrip).SequenceEqual(new[] { "导出 完成.yrrep", "导出 未完成.yrrep.partial" }.OrderBy(n => n, StringComparer.Ordinal)),
            "exported files import again and keep their completion state: " + string.Join("|", Entries(roundTrip)) + string.Join("|", SkippedReasons(imported)));

        // 拒绝的目标：与源文件相同、回放文件夹内、不存在的文件夹、同名文件夹——都不能产生任何文件。
        Check(ReplayExporter.Export(replays, finished, finishedPath, out detail) == ReplayExporter.SameAsSourceText && FileSha(finishedPath) == finishedHash,
            "exporting onto the source is refused");
        Check(ReplayExporter.Export(replays, finished, Path.Combine(replays, "copy.yrrep"), out detail) == ReplayExporter.InsideReplaysText &&
            ReplayExporter.Export(replays, finished, Path.Combine(replays, "sub", "copy.yrrep"), out detail) == ReplayExporter.InsideReplaysText &&
            Entries(replays).Length == 2, "exporting into the replays folder is refused");
        Check(ReplayExporter.Export(replays, finished, Path.Combine(outDir, "no-such-folder", "x.yrrep"), out detail).Contains("文件夹不存在"),
            "a missing destination folder is explained");
        Directory.CreateDirectory(Path.Combine(outDir, "folder.yrrep"));
        Check(ReplayExporter.Export(replays, finished, Path.Combine(outDir, "folder.yrrep"), out detail).Contains("文件夹"), "a destination that is a folder is refused");
        Check(ReplayExporter.Export(replays, null, Path.Combine(outDir, "x.yrrep"), out detail) != null, "no selection is refused");

        // 源文件在列表之后被改动、被删除、被独占。
        string changedPath = MakeReplay(Path.Combine(replays, "changed.yrrep"), "EX3", 3);
        ReplayArchive changed = ReplayArchive.Open(changedPath);
        MakeReplay(changedPath, "EX3", 4);
        string changedTarget = Path.Combine(outDir, "changed.yrrep");
        Check(ReplayExporter.Export(replays, changed, changedTarget, out detail) == ReplayExporter.ChangedSourceText && !File.Exists(changedTarget),
            "a source that changed since listing is refused");
        string vanishedPath = MakeReplay(Path.Combine(replays, "vanished.yrrep"), "EX4", 3);
        ReplayArchive vanished = ReplayArchive.Open(vanishedPath);
        File.Delete(vanishedPath);
        Check(ReplayExporter.Export(replays, vanished, Path.Combine(outDir, "vanished.yrrep"), out detail).Contains("已不存在") &&
            !File.Exists(Path.Combine(outDir, "vanished.yrrep")), "a vanished source is explained");
        string busyPath = MakeReplay(Path.Combine(replays, "busy.yrrep.partial"), "EX5", 3, true);
        ReplayArchive busy = ReplayArchive.Open(busyPath);
        using (new FileStream(busyPath, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
            Check(ReplayExporter.Export(replays, busy, Path.Combine(outDir, "busy.yrrep"), out detail) == ReplayExporter.BusySourceText &&
                !File.Exists(Path.Combine(outDir, "busy.yrrep")) && !string.IsNullOrEmpty(detail), "a source in use is explained");

        // 写入失败（磁盘已满）：目标不产生、临时文件被清理、源文件不变。
        string diskFull = Path.Combine(outDir, "disk-full.yrrep");
        string failure = ReplayExporter.Export(replays, finished, diskFull, out detail, stream => new FailingWriteStream(stream, 50), null);
        Check(failure != null && failure.Contains("磁盘空间不足") && !File.Exists(diskFull) && Directory.GetFiles(outDir, "*.exporting").Length == 0 &&
            FileSha(finishedPath) == finishedHash && !string.IsNullOrEmpty(detail), "a write failure cleans up: " + failure);
        // 写入失败发生在覆盖已有文件时：已有文件原样保留。
        string existing = Path.Combine(outDir, "existing.yrrep");
        File.WriteAllText(existing, "precious old content");
        failure = ReplayExporter.Export(replays, finished, existing, out detail, stream => new FailingWriteStream(stream, 10), null);
        Check(failure != null && File.ReadAllText(existing) == "precious old content" && Directory.GetFiles(outDir, "*.exporting").Length == 0,
            "a failed export never damages the file it was about to replace");
        // 核对失败：写出的内容与源不一致。
        string mismatch = Path.Combine(outDir, "mismatch.yrrep");
        failure = ReplayExporter.Export(replays, finished, mismatch, out detail, null, stream => new FlippingReadStream(stream));
        Check(failure == ReplayExporter.VerifyFailedText && !File.Exists(mismatch) && Directory.GetFiles(outDir, "*.exporting").Length == 0 &&
            detail.Contains("SHA-256"), "a failed verification cancels the export and cleans up");
        failure = ReplayExporter.Export(replays, finished, existing, out detail, null, stream => new FlippingReadStream(stream));
        Check(failure == ReplayExporter.VerifyFailedText && File.ReadAllText(existing) == "precious old content" &&
            Directory.GetFiles(outDir, "*.exporting").Length == 0, "a failed verification never replaces an existing file");

        // 覆盖已有文件（用户在保存窗口确认过）：原子替换，内容变为源文件，没有残留。
        Check(ReplayExporter.Export(replays, finished, existing, out detail) == null &&
            File.ReadAllBytes(existing).SequenceEqual(File.ReadAllBytes(finishedPath)) && Directory.GetFiles(outDir, "*.exporting").Length == 0,
            "an existing file is replaced with the exported content");
        Check(ReplayExporter.Export(replays, partial, existing, out detail) == null &&
            File.ReadAllBytes(existing).SequenceEqual(File.ReadAllBytes(partialPath)), "an existing file can be replaced again");
        // 目标被独占或只读：覆盖失败，原文件保留，临时文件清理。
        string lockedTarget = Path.Combine(outDir, "locked.yrrep");
        File.WriteAllText(lockedTarget, "locked old content");
        using (new FileStream(lockedTarget, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
        {
            string lockedResult = ReplayExporter.Export(replays, finished, lockedTarget, out detail);
            Check(lockedResult != null && !string.IsNullOrEmpty(detail), "replacing a locked file fails");
        }
        Check(File.ReadAllText(lockedTarget) == "locked old content" && Directory.GetFiles(outDir, "*.exporting").Length == 0, "the locked file is intact and the temporary file is gone");
        string readOnlyTarget = Path.Combine(outDir, "readonly.yrrep");
        File.WriteAllText(readOnlyTarget, "read only old content");
        File.SetAttributes(readOnlyTarget, FileAttributes.ReadOnly);
        try
        {
            Check(ReplayExporter.Export(replays, finished, readOnlyTarget, out detail) != null && File.ReadAllText(readOnlyTarget) == "read only old content" &&
                Directory.GetFiles(outDir, "*.exporting").Length == 0, "a read-only target is not replaced");
        }
        finally { File.SetAttributes(readOnlyTarget, FileAttributes.Normal); }

        // 直接测试替换原语：目标存在则被替换，源临时文件消失。
        string replaceTemp = Path.Combine(outDir, "replace.tmp");
        string replaceTarget = Path.Combine(outDir, "replace.target");
        File.WriteAllText(replaceTemp, "new");
        File.WriteAllText(replaceTarget, "old");
        ReplayExporter.ReplaceExisting(replaceTemp, replaceTarget);
        Check(File.ReadAllText(replaceTarget) == "new" && !File.Exists(replaceTemp), "ReplaceExisting replaces the target atomically");
        File.WriteAllText(replaceTemp, "again");
        Reject<IOException>(() => ReplayExporter.ReplaceExisting(replaceTemp, Path.Combine(outDir, "no-folder", "x")), "ReplaceExisting reports failures as IOException");
        Check(File.Exists(replaceTemp) && File.ReadAllText(replaceTarget) == "new", "a failed replacement keeps both files");

        // 全程没有往导出源所在的回放文件夹写入任何东西。
        Check(Entries(replays).SequenceEqual(new[] { "busy.yrrep.partial", "changed.yrrep", "crashed.yrrep.partial", "src.yrrep" }),
            "the replays folder holds only the original files: " + string.Join("|", Entries(replays)));
    }

    private static void ImportExportTests(string root)
    {
        importRoot = Path.Combine(root, "import-export");
        Directory.CreateDirectory(importRoot);
        ImportLimitDefaultTests();
        ImportClassifyTests();
        ImportNamingTests();
        ImportSummaryTests();
        ImportValidationTests();
        ImportGameSpeedTests(4);
        ImportGameSpeedTests(5);
        ImportContainerV3Tests();
        ImportZipTests();
        ImportZipLimitTests();
        ImportBrokenZipTests();
        DialogLocationTests(root);
        ExportNameTests();
        ExportDestinationTests(root);
        ExportTests(root);
    }

    // ===== 压缩旧回放 =====

    private const string ConversionSourceFixture = "replay-conversion-source-v2.yrrep";
    private const string ConversionDotnetFixture = "replay-conversion-dotnet-v3.yrrep";

    // v2 帧记录：eventCount 个事件（random 不为空时事件内容为随机字节；事件内容不参与校验），没有输入效果。
    private static byte[] ConversionRecord(uint frame, int eventCount, Random random)
    {
        byte[] record = new byte[20 + eventCount * 111];
        Buffer.BlockCopy(BitConverter.GetBytes(frame), 0, record, 0, 4);
        Buffer.BlockCopy(BitConverter.GetBytes((uint)eventCount), 0, record, 8, 4);
        if (random != null && eventCount > 0)
        {
            byte[] events = new byte[eventCount * 111];
            random.NextBytes(events);
            Buffer.BlockCopy(events, 0, record, 20, events.Length);
        }
        return record;
    }

    // v2 帧流头加帧记录；ends 为各写入单元（帧流头与每条记录）结束处的逻辑偏移。
    private static byte[] ConversionStream(IList<byte[]> records, out List<long> ends)
    {
        ends = new List<long>();
        using (MemoryStream output = new MemoryStream())
        {
            byte[] header = Frames(0, 0, 0, 2, 0).Take(32).ToArray();
            output.Write(header, 0, header.Length);
            ends.Add(output.Length);
            foreach (byte[] record in records)
            {
                output.Write(record, 0, record.Length);
                ends.Add(output.Length);
            }
            return output.ToArray();
        }
    }

    // 容器 v3 文件帧流段的各块：块头与解压结果。
    private sealed class ConversionBlock
    {
        public long Offset;
        public ReplayBlockHeader Header;
        public byte[] Raw;
    }

    private static List<ConversionBlock> BlockLayout(string path)
    {
        byte[] file = File.ReadAllBytes(path);
        List<ConversionBlock> blocks = new List<ConversionBlock>();
        int offset = 96 + BitConverter.ToInt32(file, 16);
        while (offset < file.Length)
        {
            ReplayBlockHeader header;
            byte[] raw;
            Check(ReplayBlocks.TryDecodeBlock(file, offset, file.Length - offset, ReplayBlocks.FrameStreamMaxRawBytes, out header, out raw),
                "converted block at offset " + offset + " decodes");
            blocks.Add(new ConversionBlock { Offset = offset, Header = header, Raw = raw });
            offset += (int)header.DiskBytes;
        }
        return blocks;
    }

    // 按转换器的切块策略推算各块的解压长度：追加一个单元后达到 target 字节或 maxUnits 个单元即写出；
    // 追加会使一块超过 4 MiB 时先写出当前块。
    private static List<long> ExpectedBlockSizes(List<long> ends, int target, int maxUnits)
    {
        List<long> sizes = new List<long>();
        long pending = 0;
        int units = 0;
        long previous = 0;
        foreach (long end in ends)
        {
            long size = end - previous;
            previous = end;
            if (pending > 0 && pending + size > 4 * 1024 * 1024)
            {
                sizes.Add(pending);
                pending = 0;
                units = 0;
            }
            pending += size;
            units++;
            if (pending >= target || units >= maxUnits)
            {
                sizes.Add(pending);
                pending = 0;
                units = 0;
            }
        }
        if (pending > 0) sizes.Add(pending);
        return sizes;
    }

    // 每块的结束位置都是单元边界，且最后一块结束在逻辑帧流末尾。
    private static bool BlocksOnUnitBoundaries(List<ConversionBlock> blocks, List<long> ends)
    {
        HashSet<long> boundaries = new HashSet<long>(ends);
        long position = 0;
        foreach (ConversionBlock block in blocks)
        {
            position += block.Header.RawBytes;
            if (!boundaries.Contains(position)) return false;
        }
        return position == ends[ends.Count - 1];
    }

    // 写入端规则：DeflateStream（Optimal）的结果严格小于 rawBytes 时为 method 1，否则 method 0。
    private static bool MethodFollowsRule(ConversionBlock block)
    {
        using (MemoryStream packed = new MemoryStream())
        {
            using (DeflateStream deflate = new DeflateStream(packed, CompressionLevel.Optimal, true)) deflate.Write(block.Raw, 0, block.Raw.Length);
            return block.Header.Method == (packed.Length < block.Raw.Length ? 1u : 0u);
        }
    }

    // 测试用的回收站：记下交给它的文件与内容后删除，不触碰真实回收站。
    private static ReplayConversionOptions ConversionOptions(string replays, List<KeyValuePair<string, byte[]>> recycled,
        ReplayBlockPolicy policy = null)
    {
        return new ReplayConversionOptions
        {
            ReplaysDirectory = replays,
            BlockPolicy = policy,
            MoveToRecycleBin = path =>
            {
                recycled.Add(new KeyValuePair<string, byte[]>(path, File.ReadAllBytes(path)));
                File.Delete(path);
                return null;
            }
        };
    }

    private static ReplayConversionItem ConvertFile(string path, ReplayConversionOptions options)
    {
        return ReplayConverter.ConvertOne(ReplayArchive.Open(path), options);
    }

    // 回放解压后的清单原始字节与逻辑帧流分别与 metadata、stream 逐字节相同。
    private static bool SameReplayContent(string path, byte[] metadata, byte[] stream)
    {
        ReplayArchive archive = ReplayArchive.Open(path);
        if (!archive.ReadMetadata().SequenceEqual(metadata)) return false;
        using (FileStream file = File.OpenRead(path))
        using (MemoryStream logical = new MemoryStream())
        {
            archive.OpenLogicalStream(file).CopyTo(logical);
            return logical.ToArray().SequenceEqual(stream);
        }
    }

    private static bool NoConversionLeftovers(string replays)
    {
        return Directory.GetFiles(replays).All(file => !file.EndsWith(ReplayConverter.TemporarySuffix, StringComparison.OrdinalIgnoreCase) &&
            !file.EndsWith(ReplayConverter.BackupSuffix, StringComparison.OrdinalIgnoreCase));
    }

    // 原文件未被改动：字节与最后写入时间都不变。
    private static bool Untouched(string path, byte[] bytes, DateTime written)
    {
        return File.ReadAllBytes(path).SequenceEqual(bytes) && File.GetLastWriteTimeUtc(path) == written;
    }

    private static void ConversionTests(string root)
    {
        string replays = Path.Combine(root, "conversion", "Replays");
        Directory.CreateDirectory(replays);
        byte[] manifest = ManifestWith(root, "conversion-manifest", Encoding.UTF8.GetBytes("[Settings]\nName=Conv\nSide=1\nColor=1\n"));
        byte[] gameHash = Enumerable.Range(1, 32).Select(value => (byte)value).ToArray();
        List<KeyValuePair<string, byte[]>> recycled = new List<KeyValuePair<string, byte[]>>();
        DateTime created = new DateTime(2025, 3, 4, 5, 6, 7, DateTimeKind.Utc);
        DateTime written = new DateTime(2025, 6, 7, 8, 9, 10, DateTimeKind.Utc);
        Random random = new Random(20261105);
        Func<string, byte[], string> write = (name, frameStream) =>
        {
            string path = Path.Combine(replays, name);
            Archive(path, manifest, frameStream, gameHash);
            File.SetCreationTimeUtc(path, created);
            File.SetLastWriteTimeUtc(path, written);
            return path;
        };

        // 往返（多块）：注入小切块策略；内容逐字节相同、时间戳保持、原文件经注入的回收站委托处理、块边界都在单元边界上。
        List<byte[]> records = new List<byte[]>();
        for (uint frame = 0; frame < 40; frame++) records.Add(ConversionRecord(frame, frame % 5 == 0 ? 2 : 0, frame % 10 == 0 ? random : null));
        List<long> ends;
        byte[] stream = ConversionStream(records, out ends);
        string older = write("较早.yrrep", Frames(0, 0, 0, 2, 0));
        File.SetLastWriteTimeUtc(older, written.AddDays(-1));
        string multi = write("多块.yrrep", stream);
        byte[] multiOriginal = File.ReadAllBytes(multi);
        string[] orderBefore = ReplayArchive.OpenAll(replays).Select(replay => replay.Path).ToArray();
        ReplayConversionItem item = ConvertFile(multi, ConversionOptions(replays, recycled, new ReplayBlockPolicy(300, 4)));
        ReplayArchive converted = ReplayArchive.Open(multi);
        converted.ValidatePayload();
        List<ConversionBlock> blocks = BlockLayout(multi);
        Check(item.Outcome == ReplayConversionOutcome.Converted && item.KeptBackupPath == null &&
            item.OriginalBytes == multiOriginal.Length && item.ConvertedBytes == new FileInfo(multi).Length &&
            converted.ContainerVersion == 3 && !converted.IsUnfinalized && converted.StreamVersion == 2 &&
            converted.GameExeHash.SequenceEqual(gameHash) && converted.CountFrames() == 40u &&
            SameReplayContent(multi, manifest, stream), "multi-block conversion keeps the replay byte for byte: " + item.Reason + " " + item.Detail);
        Check(blocks.Count > 5 && BlocksOnUnitBoundaries(blocks, ends) &&
            blocks.Select(block => (long)block.Header.RawBytes).SequenceEqual(ExpectedBlockSizes(ends, 300, 4)) &&
            blocks.All(MethodFollowsRule), "multi-block conversion follows the block policy");
        Check(File.GetCreationTimeUtc(multi) == created && File.GetLastWriteTimeUtc(multi) == written &&
            ReplayArchive.OpenAll(replays).Select(replay => replay.Path).SequenceEqual(orderBefore), "timestamps and list order are kept");
        Check(recycled.Count == 1 && recycled[0].Key == Path.GetFullPath(multi) + ".v2bak" && recycled[0].Value.SequenceEqual(multiOriginal) &&
            NoConversionLeftovers(replays), "the original goes to the injected recycle bin and nothing is left behind");
        Check(ReplayLaunchManifest.Hash(converted.ReadMetadata()).SequenceEqual(ReplayLaunchManifest.Hash(manifest)) &&
            new FileInfo(multi).Length < multiOriginal.Length, "converted replay is smaller and keeps the manifest");

        // 单块：生产切块策略。
        byte[] speed = SpeedStream(5, 5, 6u, null, 3u);
        string single = write("单块.yrrep", speed);
        item = ConvertFile(single, ConversionOptions(replays, recycled));
        ReplayArchive singleArchive = ReplayArchive.Open(single);
        singleArchive.ValidatePayload();
        Check(item.Outcome == ReplayConversionOutcome.Converted && BlockLayout(single).Count == 1 && singleArchive.StreamVersion == 5 &&
            singleArchive.StreamGameMode == 5u && singleArchive.CountFrames() == 3u && SameReplayContent(single, manifest, speed) &&
            File.GetLastWriteTimeUtc(single) == written && NoConversionLeftovers(replays), "single-block conversion");

        // 切块阈值：600 个单元；64 KiB；追加会超过 4 MiB 时先写出；method 0 与 1 都出现。
        records = Enumerable.Range(0, 1500).Select(frame => ConversionRecord((uint)frame, 0, null)).ToList();
        stream = ConversionStream(records, out ends);
        string units = write("600单元.yrrep", stream);
        Check(ConvertFile(units, ConversionOptions(replays, recycled)).Outcome == ReplayConversionOutcome.Converted, "600-unit file converted");
        blocks = BlockLayout(units);
        Check(blocks.Select(block => (long)block.Header.RawBytes).SequenceEqual(new long[] { 32 + 599 * 20, 600 * 20, 301 * 20 }) &&
            BlocksOnUnitBoundaries(blocks, ends) && SameReplayContent(units, manifest, stream), "600 units close a block");
        records = Enumerable.Range(0, 1300).Select(frame => ConversionRecord((uint)frame, 1, null)).ToList();
        stream = ConversionStream(records, out ends);
        string bytes64 = write("64KiB.yrrep", stream);
        Check(ConvertFile(bytes64, ConversionOptions(replays, recycled)).Outcome == ReplayConversionOutcome.Converted, "64 KiB file converted");
        blocks = BlockLayout(bytes64);
        Check(blocks.Select(block => (long)block.Header.RawBytes).SequenceEqual(new long[] { 32 + 501 * 131, 501 * 131, 298 * 131 }) &&
            blocks.Take(2).All(block => block.Header.RawBytes >= 64 * 1024) && BlocksOnUnitBoundaries(blocks, ends) &&
            SameReplayContent(bytes64, manifest, stream), "64 KiB closes a block");
        records = Enumerable.Range(0, 3).Select(frame => ConversionRecord((uint)frame, 16384, null)).ToList();
        stream = ConversionStream(records, out ends);
        string large = write("4MiB.yrrep", stream);
        Check(ConvertFile(large, ConversionOptions(replays, recycled, new ReplayBlockPolicy(4 * 1024 * 1024, 1000))).Outcome ==
            ReplayConversionOutcome.Converted, "4 MiB file converted");
        blocks = BlockLayout(large);
        Check(blocks.Select(block => (long)block.Header.RawBytes).SequenceEqual(new long[] { 32 + 2 * 1818644, 1818644 }) &&
            blocks.All(block => block.Header.RawBytes <= 4 * 1024 * 1024) && BlocksOnUnitBoundaries(blocks, ends) &&
            SameReplayContent(large, manifest, stream), "a block is written before it would exceed 4 MiB");
        records = Enumerable.Range(0, 306).Select(frame => ConversionRecord((uint)frame, frame < 6 ? 500 : 0, frame < 6 ? random : null)).ToList();
        stream = ConversionStream(records, out ends);
        string mixed = write("混合.yrrep", stream);
        Check(ConvertFile(mixed, ConversionOptions(replays, recycled)).Outcome == ReplayConversionOutcome.Converted, "mixed file converted");
        blocks = BlockLayout(mixed);
        Check(blocks.Any(block => block.Header.Method == 0) && blocks.Any(block => block.Header.Method == 1) &&
            blocks.All(MethodFollowsRule) && blocks.Select(block => (long)block.Header.RawBytes).SequenceEqual(ExpectedBlockSizes(ends, 64 * 1024, 600)) &&
            BlocksOnUnitBoundaries(blocks, ends) && SameReplayContent(mixed, manifest, stream), "method 0 and method 1 both appear and follow the rule");
        Check(NoConversionLeftovers(replays) && recycled.Count == 6, "every converted original went to the injected recycle bin");

        // 不转换：未完成录像、已是 v3、校验失败的 v2（原文件不变、不留临时文件）。
        string partial = Path.Combine(replays, "未完成.yrrep.partial");
        Partial(partial, manifest, Join(Frames(0, 0, 0, 2, 1), Effect()), gameHash);
        byte[] partialBytes = File.ReadAllBytes(partial);
        item = ConvertFile(partial, ConversionOptions(replays, recycled));
        Check(item.Outcome == ReplayConversionOutcome.Skipped && item.Reason == "未完成录像不压缩" &&
            File.ReadAllBytes(partial).SequenceEqual(partialBytes), "unfinalized replays are not converted");
        byte[] v3Bytes = File.ReadAllBytes(single);
        item = ConvertFile(single, ConversionOptions(replays, recycled));
        Check(item.Outcome == ReplayConversionOutcome.Skipped && item.Reason == "已是新格式" && File.ReadAllBytes(single).SequenceEqual(v3Bytes),
            "v3 replays are not converted again");
        string finishedPartial = write("完成但后缀为.yrrep.partial", stream.Take(32 + 20).ToArray());
        Check(!ReplayConverter.IsCandidate(ReplayArchive.Open(finishedPartial)) &&
            ConvertFile(finishedPartial, ConversionOptions(replays, recycled)).Outcome == ReplayConversionOutcome.Skipped,
            "only .yrrep files are converted");
        File.Delete(finishedPartial);
        string skipped = write("跳帧.yrrep", Frames(1, 0, 0));
        byte[] skippedBytes = File.ReadAllBytes(skipped);
        item = ConvertFile(skipped, ConversionOptions(replays, recycled));
        Check(item.Outcome == ReplayConversionOutcome.Skipped && item.Reason == "回放校验未通过，未压缩" && !string.IsNullOrEmpty(item.Detail) &&
            Untouched(skipped, skippedBytes, written) && NoConversionLeftovers(replays), "a v2 replay failing validation is left as is");
        string hashMismatch = write("哈希不符.yrrep", ConversionStream(records.Take(3).ToList(), out ends));
        byte[] tampered = File.ReadAllBytes(hashMismatch);
        tampered[tampered.Length - 1] ^= 1;
        File.WriteAllBytes(hashMismatch, tampered);
        File.SetLastWriteTimeUtc(hashMismatch, written);
        item = ConvertFile(hashMismatch, ConversionOptions(replays, recycled));
        Check(item.Outcome == ReplayConversionOutcome.Skipped && Untouched(hashMismatch, tampered, written) && NoConversionLeftovers(replays),
            "a v2 replay with a wrong content hash is left as is");

        // 核对阶段：临时文件被写坏（模拟转换代码缺陷）时放弃且删除临时文件；核对通过后原文件被改动时跳过。
        records = Enumerable.Range(0, 30).Select(frame => ConversionRecord((uint)frame, frame % 3, null)).ToList();
        stream = ConversionStream(records, out ends);
        string guarded = write("核对.yrrep", stream);
        byte[] guardedBytes = File.ReadAllBytes(guarded);
        ReplayConversionOptions corrupt = ConversionOptions(replays, recycled, new ReplayBlockPolicy(200, 3));
        corrupt.AfterWriteTemporary = temporary =>
        {
            byte[] bytes = File.ReadAllBytes(temporary);
            bytes[bytes.Length - 1] ^= 0x20;
            File.WriteAllBytes(temporary, bytes);
        };
        item = ConvertFile(guarded, corrupt);
        Check(item.Outcome == ReplayConversionOutcome.Failed && item.Reason == "压缩结果核对不一致，已放弃，原文件未改动" &&
            item.Detail.Contains("转换代码缺陷") && Untouched(guarded, guardedBytes, written) && NoConversionLeftovers(replays),
            "a damaged temporary file is caught by verification");
        ReplayConversionOptions otherContent = ConversionOptions(replays, recycled);
        otherContent.AfterWriteTemporary = temporary =>
        {
            // 换成一个自身完全有效、但清单不同的 v3 文件：Open 与 ValidatePayload 都通过，只有逐字节核对能发现。
            File.Delete(temporary);
            byte[] otherManifest = (byte[])manifest.Clone();
            otherManifest[8] ^= 1;
            using (ReplayContainerV3Writer writer = new ReplayContainerV3Writer(temporary, gameHash, otherManifest, ReplayBlockPolicy.Default))
            {
                writer.AddUnit(stream, stream.Length);
                writer.Finish();
            }
            ReplayArchive.Open(temporary).ValidatePayload();
        };
        item = ConvertFile(guarded, otherContent);
        Check(item.Outcome == ReplayConversionOutcome.Failed && item.Detail.Contains("开局清单") && Untouched(guarded, guardedBytes, written) &&
            NoConversionLeftovers(replays), "a valid but different temporary file is caught by the byte comparison");
        ReplayConversionOptions changing = ConversionOptions(replays, recycled);
        changing.BeforeReplace = path => File.SetLastWriteTimeUtc(path, written.AddSeconds(1));
        item = ConvertFile(guarded, changing);
        Check(item.Outcome == ReplayConversionOutcome.Skipped && item.Reason == ReplayConverter.ChangedText &&
            File.ReadAllBytes(guarded).SequenceEqual(guardedBytes) && NoConversionLeftovers(replays), "a file changed during conversion is skipped");
        File.SetLastWriteTimeUtc(guarded, written);
        changing.BeforeReplace = path => File.AppendAllText(path, "x");
        item = ConvertFile(guarded, changing);
        Check(item.Outcome == ReplayConversionOutcome.Skipped && item.Reason == ReplayConverter.ChangedText && NoConversionLeftovers(replays),
            "a file grown during conversion is skipped");
        File.WriteAllBytes(guarded, guardedBytes);
        File.SetLastWriteTimeUtc(guarded, written);

        // File.Replace 失败：原文件只读、原文件在替换时被占用。原文件不变，不留临时文件。
        File.SetAttributes(guarded, FileAttributes.ReadOnly);
        item = ConvertFile(guarded, ConversionOptions(replays, recycled));
        Check(item.Outcome == ReplayConversionOutcome.Failed && item.Reason.StartsWith("替换原文件失败") && item.Detail.Contains("File.Replace") &&
            Untouched(guarded, guardedBytes, written) && (File.GetAttributes(guarded) & FileAttributes.ReadOnly) != 0 &&
            NoConversionLeftovers(replays), "File.Replace failing on a read-only original leaves it unchanged: " + item.Detail);
        File.SetAttributes(guarded, FileAttributes.Normal);
        File.SetLastWriteTimeUtc(guarded, written);
        FileStream holder = null;
        ReplayConversionOptions held = ConversionOptions(replays, recycled);
        held.BeforeReplace = path => holder = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        try { item = ConvertFile(guarded, held); }
        finally { if (holder != null) holder.Dispose(); }
        Check(item.Outcome == ReplayConversionOutcome.Failed && item.Reason.StartsWith("替换原文件失败") && Untouched(guarded, guardedBytes, written) &&
            NoConversionLeftovers(replays), "File.Replace failing on an open original leaves it unchanged");

        // 回收站委托失败或抛出异常：新文件已就位，保留 .v2bak，汇总注明。
        ReplayConversionOptions refusing = ConversionOptions(replays, recycled);
        refusing.MoveToRecycleBin = path => "回收站不可用";
        item = ConvertFile(guarded, refusing);
        string backup = Path.GetFullPath(guarded) + ".v2bak";
        Check(item.Outcome == ReplayConversionOutcome.Converted && item.KeptBackupPath == backup && File.ReadAllBytes(backup).SequenceEqual(guardedBytes) &&
            ReplayArchive.Open(guarded).ContainerVersion == 3 && SameReplayContent(guarded, manifest, stream) && item.Detail.Contains("回收站不可用"),
            "a failing recycle bin keeps the .v2bak");
        ReplayConversionResult keptResult = new ReplayConversionResult(1);
        keptResult.Add(item);
        Check(keptResult.BuildSummary(10).Contains("原文件保留为 核对.yrrep.v2bak（未能移到回收站）"), "the summary names the kept .v2bak");
        // 上次在第 6、7 步之间被结束：新文件是 v3、.v2bak 是原文件；再次压缩不处理二者。
        byte[] backupBytes = File.ReadAllBytes(backup);
        ReplayConversionResult again = ReplayConverter.Convert(ReplayArchive.OpenAll(replays).Where(ReplayConverter.IsCandidate).ToList(),
            ConversionOptions(replays, recycled));
        Check(File.ReadAllBytes(backup).SequenceEqual(backupBytes) && again.Items.All(entry => entry.Path != Path.GetFullPath(guarded)) &&
            !ReplayArchive.EnumerateReplayFiles(replays).Any(file => file.EndsWith(".v2bak", StringComparison.OrdinalIgnoreCase)),
            "a left-over .v2bak is never touched and never listed");
        File.Delete(backup);
        string thrower = write("委托抛出.yrrep", stream);
        ReplayConversionOptions throwing = ConversionOptions(replays, recycled);
        throwing.MoveToRecycleBin = path => { throw new IOException("模拟的回收站错误"); };
        item = ConvertFile(thrower, throwing);
        Check(item.Outcome == ReplayConversionOutcome.Converted && item.KeptBackupPath != null && File.Exists(item.KeptBackupPath),
            "a throwing recycle bin also keeps the .v2bak");
        File.Delete(item.KeptBackupPath);

        // 同名 .v2bak 已存在（可能是用户以前的原文件）：File.Replace 会覆盖备份名，因此跳过，不碰它。
        string occupied = write("备份名已占用.yrrep", stream);
        File.WriteAllText(occupied + ".v2bak", "用户以前的原文件");
        byte[] occupiedBytes = File.ReadAllBytes(occupied);
        item = ConvertFile(occupied, ConversionOptions(replays, recycled));
        Check(item.Outcome == ReplayConversionOutcome.Skipped && File.ReadAllText(occupied + ".v2bak") == "用户以前的原文件" &&
            Untouched(occupied, occupiedBytes, written), "an existing .v2bak name is never overwritten");
        File.Delete(occupied + ".v2bak");

        // 被占用、不在回放目录下、与列表中显示的不一致：跳过。
        using (new FileStream(occupied, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            item = ConvertFile(occupied, ConversionOptions(replays, recycled));
            Check(item.Outcome == ReplayConversionOutcome.Skipped && item.Reason.Contains("占用"), "a file in use is skipped");
        }
        string nested = Path.Combine(replays, "子目录", "嵌套.yrrep");
        Directory.CreateDirectory(Path.GetDirectoryName(nested));
        Archive(nested, manifest, stream, gameHash);
        Check(ConvertFile(nested, ConversionOptions(replays, recycled)).Outcome == ReplayConversionOutcome.Skipped &&
            ReplayArchive.Open(nested).ContainerVersion == 2, "a replay outside the replays folder itself is skipped");
        ReplayArchive listed = ReplayArchive.Open(occupied);
        Archive(occupied, ManifestWith(root, "conversion-changed", Encoding.UTF8.GetBytes("[Settings]\nName=Changed\n")), stream, gameHash);
        item = ReplayConverter.ConvertOne(listed, ConversionOptions(replays, recycled));
        Check(item.Outcome == ReplayConversionOutcome.Skipped && item.Reason.Contains("已被改动") &&
            ReplayArchive.Open(occupied).ContainerVersion == 2, "a file different from the listed one is skipped");

        // 开始前清理残留的 .yrrep.v3tmp：对应的 .yrrep 仍存在时删除（原文件完好，临时文件是半成品）；
        // 对应的 .yrrep 不存在时保留（替换双重失败后可能是唯一完整的副本），并写进汇总。其他名字、子目录里的文件与 .v2bak 不动。
        File.WriteAllText(Path.Combine(replays, "a.yrrep"), "原文件仍在");
        File.WriteAllText(Path.Combine(replays, "a.yrrep.v3tmp"), "x");
        File.WriteAllText(Path.Combine(replays, "B.yrrep"), "原文件仍在");
        File.WriteAllText(Path.Combine(replays, "B.YRREP.V3TMP"), "x");
        string orphan = Path.Combine(replays, "孤立.yrrep.v3tmp");
        File.WriteAllText(orphan, "可能是唯一副本");
        File.WriteAllText(Path.Combine(replays, "c.v3tmp"), "x");
        File.WriteAllText(Path.Combine(replays, "d.yrrep.v2bak"), "x");
        File.WriteAllText(Path.Combine(replays, "子目录", "e.yrrep.v3tmp"), "x");
        ReplayConversionResult cleaned = ReplayConverter.Convert(new List<ReplayArchive>(), ConversionOptions(replays, recycled));
        Check(cleaned.CleanedTemporaryFiles == 2 && !File.Exists(Path.Combine(replays, "a.yrrep.v3tmp")) &&
            !File.Exists(Path.Combine(replays, "B.YRREP.V3TMP")) && File.Exists(Path.Combine(replays, "a.yrrep")) &&
            File.Exists(Path.Combine(replays, "B.yrrep")) && File.Exists(Path.Combine(replays, "c.v3tmp")) &&
            File.Exists(Path.Combine(replays, "d.yrrep.v2bak")) && File.Exists(Path.Combine(replays, "子目录", "e.yrrep.v3tmp")),
            "left-over .yrrep.v3tmp files whose .yrrep still exists are removed before converting");
        Check(File.ReadAllText(orphan) == "可能是唯一副本" && cleaned.KeptTemporaryFiles.SequenceEqual(new[] { orphan }) &&
            cleaned.BuildSummary(10).EndsWith("发现 1 个无法确认的临时文件（孤立.yrrep.v3tmp），可能是上次压缩失败后留下的唯一副本，已保留，请自行检查。"),
            "a .yrrep.v3tmp without its .yrrep is kept and reported: " + cleaned.BuildSummary(10));
        ReplayConversionResult rerun = ReplayConverter.Convert(new List<ReplayArchive>(), ConversionOptions(replays, recycled));
        Check(File.Exists(orphan) && rerun.CleanedTemporaryFiles == 0 && rerun.KeptTemporaryFiles.SequenceEqual(new[] { orphan }),
            "the kept .yrrep.v3tmp survives later runs");
        File.WriteAllText(Path.Combine(replays, "孤立.yrrep"), "原文件已恢复");
        ReplayConversionResult restored = ReplayConverter.Convert(new List<ReplayArchive>(), ConversionOptions(replays, recycled));
        Check(!File.Exists(orphan) && restored.CleanedTemporaryFiles == 1 && restored.KeptTemporaryFiles.Count == 0 &&
            !restored.BuildSummary(10).Contains("临时文件"), "once its .yrrep exists again the left-over is removed");
        ReplayConversionResult manyKept = new ReplayConversionResult(0);
        for (int i = 0; i < 4; i++) manyKept.KeptTemporaryFiles.Add(Path.Combine(replays, "k" + i + ".yrrep.v3tmp"));
        Check(manyKept.BuildSummary(2).Contains("发现 4 个无法确认的临时文件（k0.yrrep.v3tmp、k1.yrrep.v3tmp 等 2 个），"),
            "many kept temporary files are listed briefly");
        Check(!ReplayArchive.EnumerateReplayFiles(replays).Any(file => file.EndsWith(".v3tmp", StringComparison.OrdinalIgnoreCase) ||
            file.EndsWith(".v2bak", StringComparison.OrdinalIgnoreCase)), ".v3tmp and .v2bak are not listed as replays");
        foreach (string leftover in new[] { "a.yrrep", "B.yrrep", "孤立.yrrep", "c.v3tmp", "d.yrrep.v2bak" })
            File.Delete(Path.Combine(replays, leftover));

        // 停止请求在文件之间生效：正在处理的文件做完，其余不处理；进度从 1 起报告。
        string[] queue = { write("队列1.yrrep", stream), write("队列2.yrrep", stream), write("队列3.yrrep", stream) };
        List<int> progress = new List<int>();
        ReplayConversionOptions stopping = ConversionOptions(replays, recycled);
        stopping.Progress = (index, total) => { if (total == 3) progress.Add(index); };
        stopping.StopRequested = () => progress.Count >= 1;
        ReplayConversionResult stopped = ReplayConverter.Convert(queue.Select(path => ReplayArchive.Open(path)).ToList(), stopping);
        Check(stopped.Stopped && stopped.NotProcessed == 2 && stopped.Items.Count == 1 &&
            stopped.Items[0].Outcome == ReplayConversionOutcome.Converted && progress.SequenceEqual(new[] { 1 }) &&
            ReplayArchive.Open(queue[0]).ContainerVersion == 3 && ReplayArchive.Open(queue[1]).ContainerVersion == 2 &&
            ReplayArchive.Open(queue[2]).ContainerVersion == 2 && stopped.BuildSummary(10).Contains("已按要求停止，还有 2 个回放未处理。"),
            "a stop request takes effect between files");
        ReplayConversionOptions all = ConversionOptions(replays, recycled);
        all.Progress = (index, total) => progress.Add(index * 10 + total);
        progress.Clear();
        ReplayConversionResult rest = ReplayConverter.Convert(queue.Skip(1).Select(path => ReplayArchive.Open(path)).ToList(), all);
        Check(!rest.Stopped && rest.Items.Count(entry => entry.Outcome == ReplayConversionOutcome.Converted) == 2 &&
            progress.SequenceEqual(new[] { 12, 22 }), "the remaining files convert and report progress");
        Reject<ArgumentException>(() => ReplayConverter.Convert(new List<ReplayArchive>(), new ReplayConversionOptions { ReplaysDirectory = replays }),
            "conversion without a recycle bin operation is refused");

        // 汇总文字。
        ReplayConversionResult summary = new ReplayConversionResult(5);
        summary.Add(new ReplayConversionItem { Path = "a.yrrep", Outcome = ReplayConversionOutcome.Converted, OriginalBytes = 10 * 1024 * 1024, ConvertedBytes = 1024 * 1024 });
        summary.Add(new ReplayConversionItem { Path = "b.yrrep", Outcome = ReplayConversionOutcome.Converted, OriginalBytes = 2 * 1024 * 1024, ConvertedBytes = 200 * 1024 });
        summary.Add(new ReplayConversionItem { Path = "c.yrrep", Outcome = ReplayConversionOutcome.Skipped, Reason = "回放校验未通过，未压缩" });
        summary.Add(new ReplayConversionItem { Path = "d.yrrep", Outcome = ReplayConversionOutcome.Failed, Reason = "写入临时文件时出错，可能磁盘空间不足" });
        summary.MarkStopped(1);
        string summaryText = summary.BuildSummary(10);
        string[] summaryLines = summaryText.Split(new[] { Environment.NewLine }, StringSplitOptions.None);
        Check(summaryLines[0] == "已压缩 2 个回放：12.0 MB → 1.2 MB。" && summaryLines[1] == "原文件已移到回收站，清空回收站后可释放约 12.0 MB。" &&
            summaryText.Contains("已按要求停止，还有 1 个回放未处理。") && summaryText.Contains("跳过 1 个，失败 1 个（这些回放保持原样）：") &&
            summaryText.Contains("- d.yrrep：写入临时文件时出错，可能磁盘空间不足") && summaryText.Contains("- c.yrrep：回放校验未通过，未压缩"),
            "conversion summary: " + summaryText);
        ReplayConversionResult many = new ReplayConversionResult(12);
        for (int i = 0; i < 12; i++) many.Add(new ReplayConversionItem { Path = i + ".yrrep", Outcome = ReplayConversionOutcome.Skipped, Reason = "已是新格式" });
        string manyText = many.BuildSummary(10);
        Check(manyText.StartsWith("没有压缩任何回放。") && manyText.Contains("等 2 个（未全部列出）") && !manyText.Contains("回收站"),
            "summary lists at most 10 reasons");
        Console.WriteLine("压缩旧回放检查通过");
    }

    // 带有效 RPLM 开局清单的固定清单字节（样例必须可重复生成）。
    private static byte[] ConversionFixtureManifest()
    {
        byte[] spawn = Encoding.UTF8.GetBytes("[Settings]\r\nName=Sample\r\nGameMode=5\r\nSeed=305419896\r\n");
        byte[] spawnMap = Encoding.UTF8.GetBytes("[Map]\r\nName=压缩转换样例\r\n");
        byte[] mapPath = Encoding.UTF8.GetBytes("Maps\\conversion-sample.map");
        byte[] mapName = Encoding.UTF8.GetBytes("压缩转换样例");
        using (MemoryStream output = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(output))
        {
            writer.Write(0x4D4C5052u);
            writer.Write(1u);
            writer.Write(1762300800L); // 2025-11-05 00:00:00 UTC
            writer.Write((uint)mapPath.Length);
            writer.Write(mapPath);
            writer.Write((uint)mapName.Length);
            writer.Write(mapName);
            writer.Write(Hash(Encoding.ASCII.GetBytes("conversion sample base map")));
            writer.Write(Hash(spawnMap));
            writer.Write((uint)spawn.Length);
            writer.Write(spawn);
            writer.Write((uint)spawnMap.Length);
            writer.Write(spawnMap);
            writer.Flush();
            byte[] bytes = output.ToArray();
            ReplayLaunchManifest.Deserialize(bytes);
            return bytes;
        }
    }

    private static string HexSha(byte[] data)
    {
        return BitConverter.ToString(Hash(data)).Replace("-", "").ToLowerInvariant();
    }

    // 压缩旧回放的跨语言样例：转换结果能 Open 与 ValidatePayload，内容与源样例相同；
    // 用本类库的转换器再转换源样例，清单、逻辑帧流与块布局须与转换结果样例相同（文件字节是否相同只作说明）。
    private static void ConversionFixtureTests(string root, string fixtureDirectory)
    {
        string sourcePath = Path.Combine(fixtureDirectory, ConversionSourceFixture);
        string convertedPath = Path.Combine(fixtureDirectory, ConversionDotnetFixture);
        foreach (string required in new[] { sourcePath, convertedPath })
        {
            if (File.Exists(required)) continue;
            string missing = "缺少压缩旧回放跨语言样例：" + required +
                "。样例跟踪在 tests/fixtures 中，测试不重新生成，不能跳过。";
            Console.Error.WriteLine("FAIL " + missing);
            throw new Exception(missing);
        }
        byte[] source = File.ReadAllBytes(sourcePath);
        byte[] converted = File.ReadAllBytes(convertedPath);
        // 样例只生成一次并跟踪：长度与 SHA-256 固定（2026-11-05 生成），块布局见下方。
        Check(source.Length == 803 && HexSha(source) == "89d0e8b528bd0f14376c333643140809be5cc79ca54b6b34e32cb67b1a852ad6" &&
            converted.Length == 500 && HexSha(converted) == "cc9732ca868a3f1e6b0a4cf03ce1706f383e4bd585725df9c9e94ba188ff514e",
            "conversion fixtures keep their tracked bytes");
        Check(BitConverter.ToUInt32(source, 8) == 2 && BitConverter.ToUInt32(source, 16) == 225 && BitConverter.ToUInt64(source, 24) == 482 &&
            BitConverter.ToUInt32(converted, 8) == 3 && BitConverter.ToUInt32(converted, 16) == 219 && BitConverter.ToUInt64(converted, 24) == 185 &&
            BitConverter.ToString(converted, 64, 32).Replace("-", "").ToLowerInvariant() ==
                "75ac4ba4da3063692f36566fed83a6f3e229c383dcc994e4df09517d6cb89d0b",
            "conversion fixture container headers");
        ReplayBlockHeader manifestBlock;
        byte[] manifestRaw;
        Check(ReplayBlocks.TryDecodeBlock(converted, 96, 219, ReplayBlocks.ManifestMaxRawBytes, out manifestBlock, out manifestRaw) &&
            manifestBlock.Method == 1 && manifestBlock.RawBytes == 225 && manifestBlock.StoredBytes == 199 && manifestBlock.Crc32 == 0xEEE5CE3Au &&
            manifestRaw.SequenceEqual(source.Skip(96).Take(225)), "converted fixture manifest block holds the original metadata bytes");
        List<ConversionBlock> fixtureBlocks = BlockLayout(convertedPath);
        Check(fixtureBlocks.Select(block => block.Offset).SequenceEqual(new long[] { 315, 390, 454 }) &&
            fixtureBlocks.All(block => block.Header.Method == 1) &&
            fixtureBlocks.Select(block => block.Header.StoredBytes).SequenceEqual(new uint[] { 55, 44, 26 }) &&
            fixtureBlocks.Select(block => block.Header.Crc32).SequenceEqual(new uint[] { 0x5EEAAB7A, 0x223FA013, 0x467749F0 }),
            "converted fixture block layout");
        ReplayArchive sourceArchive = ReplayArchive.Open(sourcePath);
        ReplayArchive convertedArchive = ReplayArchive.Open(convertedPath);
        sourceArchive.ValidatePayload();
        convertedArchive.ValidatePayload();
        byte[] v5 = File.ReadAllBytes(Path.Combine(fixtureDirectory, "replay-v5-speed.yrrep"));
        byte[] v5Stream = v5.Skip(96 + BitConverter.ToInt32(v5, 16)).ToArray();
        byte[] gameHash = Enumerable.Range(1, 32).Select(value => (byte)value).ToArray();
        Check(sourceArchive.ContainerVersion == 2 && convertedArchive.ContainerVersion == 3 && !sourceArchive.IsUnfinalized &&
            !convertedArchive.IsUnfinalized && sourceArchive.StreamVersion == 5 && convertedArchive.StreamVersion == 5 &&
            sourceArchive.GameExeHash.SequenceEqual(gameHash) && convertedArchive.GameExeHash.SequenceEqual(gameHash) &&
            sourceArchive.CountFrames() == 6u && convertedArchive.CountFrames() == 6u && sourceArchive.Manifest.MapName == "压缩转换样例",
            "conversion fixtures open and validate as v2 and v3");
        Check(sourceArchive.ReadMetadata().SequenceEqual(ConversionFixtureManifest()) &&
            convertedArchive.ReadMetadata().SequenceEqual(sourceArchive.ReadMetadata()) &&
            SameReplayContent(sourcePath, ConversionFixtureManifest(), v5Stream) && SameReplayContent(convertedPath, ConversionFixtureManifest(), v5Stream),
            "conversion fixtures carry the same manifest bytes and the v5 sample frames");
        List<ConversionBlock> blocks = BlockLayout(convertedPath);
        Check(blocks.Select(block => block.Header.RawBytes).SequenceEqual(new uint[] { 219, 116, 147 }),
            "converted fixture uses three units per block");
        // 用当前转换器再转一次：清单与逻辑帧流须与转换结果样例逐字节相同，块布局（各块解压长度与 CRC-32）须相同，不符即失败。
        // 文件字节取决于 deflate 实现，系统更新换了 zlib 版本时可能不同：逐字节是否相同只输出说明，不计失败，
        // 常规构建与测试不因此失败。
        string replays = Path.Combine(root, "conversion-fixture-check");
        Directory.CreateDirectory(replays);
        string again = Path.Combine(replays, "again.yrrep");
        File.Copy(sourcePath, again);
        ReplayConversionItem item = ReplayConverter.ConvertOne(ReplayArchive.Open(again), new ReplayConversionOptions
        {
            ReplaysDirectory = replays,
            BlockPolicy = new ReplayBlockPolicy(64 * 1024, 3),
            MoveToRecycleBin = path => { File.Delete(path); return null; }
        });
        Check(item.Outcome == ReplayConversionOutcome.Converted && SameReplayContent(again, ConversionFixtureManifest(), v5Stream) &&
            BlockLayout(again).Select(block => block.Header.RawBytes).SequenceEqual(new uint[] { 219, 116, 147 }),
            "converting the source fixture again gives the same content");
        byte[] fixtureLogical;
        using (FileStream file = File.OpenRead(convertedPath))
        using (MemoryStream logical = new MemoryStream())
        {
            convertedArchive.OpenLogicalStream(file).CopyTo(logical);
            fixtureLogical = logical.ToArray();
        }
        List<ConversionBlock> againBlocks = BlockLayout(again);
        Check(SameReplayContent(again, convertedArchive.ReadMetadata(), fixtureLogical) &&
            againBlocks.Select(block => block.Header.RawBytes).SequenceEqual(fixtureBlocks.Select(block => block.Header.RawBytes)) &&
            againBlocks.Select(block => block.Header.Crc32).SequenceEqual(fixtureBlocks.Select(block => block.Header.Crc32)),
            "converting the source fixture again matches the dotnet fixture's manifest, logical frame stream and block layout");
        Console.WriteLine("压缩旧回放样例：源 SHA-256 " + HexSha(source) + "（" + source.Length + " 字节），转换结果 SHA-256 " +
            HexSha(converted) + "（" + converted.Length + " 字节）");
        byte[] reconverted = File.ReadAllBytes(again);
        Console.WriteLine("压缩旧回放样例重新转换（" + reconverted.Length + " 字节，SHA-256 " + HexSha(reconverted) + "）：" +
            (reconverted.SequenceEqual(converted) ? "与样例逐字节相同" : "字节不同：deflate 实现不同，内容一致"));
        Console.WriteLine("压缩旧回放样例核对通过");
    }

    private static void Main(string[] args)
    {
        // 参数：第一个不以 "--" 开头的是夹具目录；--recycle-bin-integration 显式开启真实回收站用例（默认不传）。
        bool runRecycleBin = args.Contains("--recycle-bin-integration");
        string fixtureDirectory = args.FirstOrDefault(argument => !argument.StartsWith("--", StringComparison.Ordinal));
        string root = Path.Combine(Path.GetTempPath(), "replay-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            string maps = Path.Combine(root, "Maps");
            Directory.CreateDirectory(maps);
            string map = Path.Combine(maps, "地图.map");
            File.WriteAllText(map, "base map", Encoding.UTF8);
            byte[] spawn = Encoding.GetEncoding(28591).GetBytes("[Settings]\r\nHost=Yes\r\nPort=1234\r\n[Other1]\r\nIp=203.0.113.1\r\nPort=4444\r\n[Tunnel]\r\nIp=203.0.113.2\r\nPort=5555\r\n[Multi1]\r\nColor=2\r\n");
            byte[] finalMap = Encoding.UTF8.GetBytes("[Map]\r\nName=地图\r\n");
            File.WriteAllBytes(Path.Combine(root, "spawn.ini"), spawn);
            File.WriteAllBytes(Path.Combine(root, "spawnmap.ini"), finalMap);
            ReplayLaunchManifest manifest = ReplayLaunchManifest.Create(root, map, "地图");
            byte[] encoded = manifest.Serialize();
            ReplayLaunchManifest decoded = ReplayLaunchManifest.Deserialize(encoded);
            Check(decoded.MapName == "地图" && decoded.FindBaseMap(root) == map, "manifest roundtrip and map lookup");
            Check(Convert.ToBase64String(decoded.SpawnIniBytes) == Convert.ToBase64String(spawn), "spawn bytes roundtrip");

            byte[] local = PlaybackSession.MakeLocalEndpoints(spawn);
            string localText = Encoding.GetEncoding(28591).GetString(local);
            Check(localText.Contains("Host=Yes") && localText.Contains("Color=2") &&
                localText.Contains("Ip=127.0.0.1") && localText.Contains("Port=9") &&
                localText.Contains("[Settings]\r\nHost=Yes\r\nPort=0\r\n"), "endpoint-only rewrite");

            byte[] frames = Frames(0, 0, 0);
            string archivePath = Path.Combine(root, "sample.yrrep");
            Archive(archivePath, encoded, frames);
            ReplayArchive opened = ReplayArchive.Open(archivePath);
            Check(opened.StreamVersion == 2 && opened.HasPlayerInputEffects, "v2 capability exposed");
            Check(!opened.IsUnfinalized && opened.RecoveredFrameCount == 0 && opened.StreamBytes == frames.Length &&
                opened.MatchesCurrentHeader(), "finished archive keeps strict header semantics");
            opened.ValidatePayload();
            checks++;
            byte[] broken = File.ReadAllBytes(archivePath);
            broken[broken.Length - 1] ^= 1;
            File.WriteAllBytes(archivePath, broken);
            Reject<InvalidDataException>(() => ReplayArchive.Open(archivePath).ValidatePayload(), "payload hash mismatch");
            byte[] truncated = new byte[broken.Length - 1];
            Buffer.BlockCopy(broken, 0, truncated, 0, truncated.Length);
            File.WriteAllBytes(archivePath, truncated);
            Reject<InvalidDataException>(() => ReplayArchive.Open(archivePath), "truncated container");
            Archive(archivePath, encoded, frames);
            Archive(archivePath, encoded, Frames(1, 0, 0));
            Reject<InvalidDataException>(() => ReplayArchive.Open(archivePath).ValidatePayload(), "skipped frame");
            Archive(archivePath, encoded, Frames(0, 1, 0));
            Reject<InvalidDataException>(() => ReplayArchive.Open(archivePath).ValidatePayload(), "truncated event");
            Archive(archivePath, encoded, Frames(0, 0, 2));
            Reject<InvalidDataException>(() => ReplayArchive.Open(archivePath).ValidatePayload(), "invalid frame flags");
            Archive(archivePath, encoded, Join(Frames(0, 0, 0, 2, 1), Effect()));
            ReplayArchive.Open(archivePath).ValidatePayload();
            checks++;
            Archive(archivePath, encoded, Frames(0, 0, 0, 2, 1));
            Reject<InvalidDataException>(() => ReplayArchive.Open(archivePath).ValidatePayload(), "truncated effect");
            Archive(archivePath, encoded, Join(Frames(0, 0, 0, 2, 1), Effect(2)));
            Reject<InvalidDataException>(() => ReplayArchive.Open(archivePath).ValidatePayload(), "invalid effect kind");
            Archive(archivePath, encoded, Join(Frames(0, 0, 0, 2, 1), Effect(1, 2)));
            Reject<InvalidDataException>(() => ReplayArchive.Open(archivePath).ValidatePayload(), "invalid effect boolean");
            Archive(archivePath, encoded, Frames(0, 0, 0, 2, 4097));
            Reject<InvalidDataException>(() => ReplayArchive.Open(archivePath).ValidatePayload(), "effect count limit");
            Archive(archivePath, encoded, Frames(0, 0, 0, 1));
            ReplayArchive legacyArchive = ReplayArchive.Open(archivePath);
            Check(legacyArchive.StreamVersion == 1 && !legacyArchive.HasPlayerInputEffects,
                "legacy capability exposed");
            legacyArchive.ValidatePayload();
            checks++;
            // 宿主使用的 ReplayClient.PreparePlayback 要求游戏目录有插件文件；旧格式必须在写入任何启动文件之前被明确拒绝。
            File.WriteAllText(Path.Combine(root, "RAReplayPlugin.dll"), "placeholder");
            try
            {
                ReplayClient.PreparePlayback(root, archivePath, Path.Combine(root, "gamemd.exe"), manifestToMap => null);
                throw new Exception("legacy replay prepared unexpectedly");
            }
            catch (InvalidDataException error)
            {
                Check(error.Message.Contains("旧格式缺少玩家输入效果，请重新录制"),
                    "legacy replay rejected clearly before launch");
            }
            Archive(archivePath, encoded, frames);

            byte[] badPath = Encoding.UTF8.GetBytes("..\\outside.map");
            using (MemoryStream payload = new MemoryStream())
            using (BinaryWriter writer = new BinaryWriter(payload))
            {
                writer.Write(0x4D4C5052u);
                writer.Write(1u);
                writer.Write(manifest.RecordedAtUnixSeconds);
                writer.Write((uint)badPath.Length);
                writer.Write(badPath);
                byte[] name = Encoding.UTF8.GetBytes(manifest.MapName);
                writer.Write((uint)name.Length);
                writer.Write(name);
                writer.Write(manifest.BaseMapHash);
                writer.Write(manifest.SpawnMapHash);
                writer.Write((uint)spawn.Length);
                writer.Write(spawn);
                writer.Write((uint)finalMap.Length);
                writer.Write(finalMap);
                Reject<InvalidDataException>(() => ReplayLaunchManifest.Deserialize(payload.ToArray()).FindBaseMap(root), "path escape");
            }

            ReplayRuntimeRequest request = ReplayRuntimeRequest.Create(ReplayRuntimeRequest.Record,
                Path.Combine(root, "Replays", "中文.yrrep"), encoded);
            byte[] requestBytes = request.Serialize();
            ReplayRuntimeRequest reread = ReplayRuntimeRequest.Deserialize(requestBytes);
            Check(reread.Mode == ReplayRuntimeRequest.Record && reread.ReplayPath == request.ReplayPath &&
                Convert.ToBase64String(reread.RequestId) == Convert.ToBase64String(request.RequestId), "request codec");
            byte[] reserved = (byte[])requestBytes.Clone();
            reserved[12] = 1;
            Reject<InvalidDataException>(() => ReplayRuntimeRequest.Deserialize(reserved), "reserved bits");

            string marker = Path.Combine(root, "rareplay-runtime.bin");
            byte[] staleRecord = (byte[])requestBytes.Clone();
            Buffer.BlockCopy(BitConverter.GetBytes(DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - 6 * 60 * 1000),
                0, staleRecord, 16, 8);
            File.WriteAllBytes(marker, staleRecord);
            ReplayRuntimeRequest.ClearStale(root);
            Check(!File.Exists(marker), "stale record cleared before playback");
            byte[] stalePlay = ReplayRuntimeRequest.Create(ReplayRuntimeRequest.Play,
                Path.Combine(root, "Replays", "中文.yrrep"), new byte[0]).Serialize();
            Buffer.BlockCopy(BitConverter.GetBytes(DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - 6 * 60 * 1000),
                0, stalePlay, 16, 8);
            File.WriteAllBytes(marker, stalePlay);
            ReplayRuntimeRequest.ClearStale(root);
            Check(!File.Exists(marker), "stale play cleared before recording");
            File.WriteAllBytes(marker, requestBytes);
            ReplayRuntimeRequest.ClearStale(root);
            Check(File.Exists(marker), "fresh request retained");

            // 未完成录像（容器头 flags==0）：以 flags 判定，与后缀无关；磁盘上的 .partial 路径同样可播放。
            byte[] frameZero = Join(Frames(0, 0, 0, 2, 1), Effect());
            byte[] frameOne = Frames(1, 0, 0, 2, 0).Skip(32).ToArray();
            byte[] frameTwo = Frames(2, 0, 0, 2, 0).Skip(32).ToArray();
            byte[] partialStream = Join(frameZero, frameOne);
            string partialPath = Path.Combine(root, "crashed.yrrep.partial");
            Partial(partialPath, encoded, partialStream);
            ReplayArchive recovered = ReplayArchive.Open(partialPath);
            Check(recovered.IsUnfinalized && recovered.RecoveredFrameCount == 2 && recovered.StreamBytes == partialStream.Length &&
                recovered.StreamVersion == 2 && recovered.HasPlayerInputEffects && recovered.Manifest.MapName == "地图" &&
                recovered.Path == Path.GetFullPath(partialPath), "unfinalized archive recovered");
            recovered.ValidatePayload();
            checks++;
            Check(recovered.MatchesCurrentHeader(), "unfinalized header matches unchanged file");
            string finishedName = Path.Combine(root, "flags-zero.yrrep");
            File.Copy(partialPath, finishedName);
            Check(ReplayArchive.Open(finishedName).IsUnfinalized, "flags decide unfinalized, not .yrrep suffix");
            string arbitraryName = Path.Combine(root, "flags-zero.bin");
            File.Copy(partialPath, arbitraryName);
            Check(ReplayArchive.Open(arbitraryName).IsUnfinalized, "flags decide unfinalized, not arbitrary suffix");
            string finishedPartial = Path.Combine(root, "flags-one.yrrep.partial");
            Archive(finishedPartial, encoded, partialStream);
            ReplayArchive finishedByFlags = ReplayArchive.Open(finishedPartial);
            Check(!finishedByFlags.IsUnfinalized && finishedByFlags.RecoveredFrameCount == 0 &&
                finishedByFlags.StreamBytes == partialStream.Length, "flags decide finished, not .partial suffix");
            finishedByFlags.ValidatePayload();
            checks++;

            // 未完成录像在打开后被改动：可恢复长度、帧数或清单变化都必须被 ValidatePayload 与 MatchesCurrentHeader 发现。
            byte[] partialBytes = File.ReadAllBytes(partialPath);
            File.WriteAllBytes(partialPath, Join(partialBytes, new byte[3]));
            recovered.ValidatePayload();
            checks++;
            Check(recovered.MatchesCurrentHeader(), "unrecoverable tail does not change recoverable content");
            File.WriteAllBytes(partialPath, Join(partialBytes, frameTwo));
            Reject<InvalidDataException>(() => recovered.ValidatePayload(), "appended frame detected by payload validation");
            Check(!recovered.MatchesCurrentHeader(), "appended frame detected by header match");
            File.WriteAllBytes(partialPath, partialBytes.Take(partialBytes.Length - 1).ToArray());
            Reject<InvalidDataException>(() => recovered.ValidatePayload(), "dropped frame detected by payload validation");
            Check(!recovered.MatchesCurrentHeader(), "dropped frame detected by header match");
            byte[] manifestChanged = (byte[])partialBytes.Clone();
            manifestChanged[96 + 8] ^= 1; // RecordedAtUnixSeconds 最低字节，清单仍可解析
            File.WriteAllBytes(partialPath, manifestChanged);
            Reject<InvalidDataException>(() => recovered.ValidatePayload(), "changed manifest detected by payload validation");
            Check(!recovered.MatchesCurrentHeader(), "changed manifest detected by header match");
            byte[] exeChanged = (byte[])partialBytes.Clone();
            exeChanged[32] ^= 1;
            File.WriteAllBytes(partialPath, exeChanged);
            Check(!recovered.MatchesCurrentHeader(), "changed game hash detected by header match");
            byte[] corruptedFrame = (byte[])partialBytes.Clone();
            corruptedFrame[corruptedFrame.Length - 20] ^= 1; // 帧 1 的帧号，可恢复帧数变化
            File.WriteAllBytes(partialPath, corruptedFrame);
            Reject<InvalidDataException>(() => recovered.ValidatePayload(), "corrupted recoverable frame detected");
            File.WriteAllBytes(partialPath, partialBytes);
            recovered.ValidatePayload();
            checks++;
            Check(recovered.MatchesCurrentHeader(), "restored unfinalized file matches again");

            // 正在录制的 .partial 被写入方独占，打开失败；列表据此静默跳过。
            using (new FileStream(partialPath, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
                Reject<IOException>(() => ReplayArchive.Open(partialPath), "exclusively locked partial cannot be opened");
            Check(ReplayArchive.Open(partialPath).IsUnfinalized, "partial opens again after the writer releases it");

            // 旧格式（V1）的未完成录像与完成文件一样，启动前就被明确拒绝。
            string legacyPartialPath = Path.Combine(root, "legacy.yrrep.partial");
            Partial(legacyPartialPath, encoded, Frames(0, 0, 0, 1));
            ReplayArchive legacyPartial = ReplayArchive.Open(legacyPartialPath);
            Check(legacyPartial.IsUnfinalized && legacyPartial.StreamVersion == 1 && !legacyPartial.HasPlayerInputEffects,
                "legacy unfinalized archive recovered");
            Reject<InvalidDataException>(() => ReplayClient.PreparePlayback(root, legacyPartialPath,
                Path.Combine(root, "gamemd.exe"), manifestToMap => null), "legacy unfinalized replay rejected before launch");

            // 回放列表发现文件：*.yrrep 与 *.yrrep.partial，后缀大小写不敏感，其他扩展名一律排除。
            string listing = Path.Combine(root, "Listing");
            Directory.CreateDirectory(listing);
            foreach (string fileName in new[] { "a.yrrep", "b.yrrep.partial", "C.YRREP", "d.YRREP.PARTIAL", "e.yrrepx",
                "f.yrrep.partial.bak", "g.yrrep.tmp", "h.yrrep.partialx", "i.partial", "j.txt", "k.yrrep.part" })
                File.WriteAllBytes(Path.Combine(listing, fileName), new byte[0]);
            string[] found = ReplayArchive.EnumerateReplayFiles(listing).Select(file => Path.GetFileName(file))
                .OrderBy(sorted => sorted, StringComparer.Ordinal).ToArray();
            string[] expectedNames = new[] { "C.YRREP", "a.yrrep", "b.yrrep.partial", "d.YRREP.PARTIAL" };
            Check(found.SequenceEqual(expectedNames), "replay discovery matches .yrrep and .yrrep.partial exactly");

            // 播放请求与过期清理不假设路径以 .yrrep 结尾。
            string partialRequestPath = Path.Combine(root, "Replays", "录制中止.yrrep.partial");
            byte[] partialRequest = ReplayRuntimeRequest.Create(ReplayRuntimeRequest.Play, partialRequestPath,
                new byte[0]).Serialize();
            Check(ReplayRuntimeRequest.Deserialize(partialRequest).ReplayPath == Path.GetFullPath(partialRequestPath),
                "play request keeps partial path");
            Buffer.BlockCopy(BitConverter.GetBytes(DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - 6 * 60 * 1000),
                0, partialRequest, 16, 8);
            File.WriteAllBytes(marker, partialRequest);
            ReplayRuntimeRequest.ClearStale(root);
            Check(!File.Exists(marker), "stale partial play request cleared");

            BufferedScanTests(root, encoded);
            OpenAllTests(root, encoded, frames, partialStream);
            DisplaySummaryTests();
            DetailsAndFrameCountTests(root);
            ContainerV3DetailsTests(root);
            ConversionTests(root);
            DeletePathRuleTests(root);
            DeleteFlowTests(root);
            ImportExportTests(root);
            if (runRecycleBin) RecycleBinIntegrationTests(root);
            else Console.WriteLine("回收站集成用例已跳过（需显式参数开启）");

            // 已存在请求使最后一步发布失败；两个 spawn 文件必须恢复到原字节。
            File.WriteAllBytes(marker, new byte[] { 1 });
            Reject<IOException>(() => PlaybackSession.Prepare(root, ReplayArchive.Open(archivePath), 0u, false), "publish failure");
            Check(Convert.ToBase64String(File.ReadAllBytes(Path.Combine(root, "spawn.ini"))) == Convert.ToBase64String(spawn) &&
                Convert.ToBase64String(File.ReadAllBytes(Path.Combine(root, "spawnmap.ini"))) == Convert.ToBase64String(finalMap), "restore rollback");
            Console.WriteLine("Replay tests passed: " + checks);
            FrameStreamValidationTests.Run(root, fixtureDirectory);
            HostLifecycleTests.Run(Path.Combine(root, "host-lifecycle"));
            Console.WriteLine("宿主接入生命周期检查通过");
            // 压缩旧回放样例必须存在：缺少时明确失败（放在最后，其余检查先全部执行）。
            if (!string.IsNullOrEmpty(fixtureDirectory))
            {
                ConversionFixtureTests(root, fixtureDirectory);
                Console.WriteLine("Replay tests passed: " + checks);
            }
        }
        finally { Directory.Delete(root, true); }
    }
}
