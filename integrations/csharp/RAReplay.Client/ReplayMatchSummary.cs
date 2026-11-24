using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace RAReplay.Client
{
    public sealed class ReplayPlayerRow
    {
        public string Name { get; set; }
        public bool IsAi { get; set; }
        public bool IsRecorder { get; set; }
        public bool IsSpectator { get; set; }
        public string SideText { get; set; }
        public string ColorText { get; set; }
        public bool HasColor { get; set; }
        public byte ColorR { get; set; }
        public byte ColorG { get; set; }
        public byte ColorB { get; set; }
        public string StartText { get; set; }
        public string AllianceText { get; set; }
    }

    // 清单内 spawn.ini 的展示摘要。只读取 DTAClient 的 WriteSpawnIni / AllianceHolder 明确写入的字段：
    // [Settings] 的 Name/Side/Color/IsSpectator/UIGameMode/PlayerCount/AIPlayers 与大厅选项键，
    // [OtherN] 的 Name/Side/Color/IsSpectator，[HouseHandicaps]/[HouseCountries]/[HouseColors] 的电脑玩家，
    // [SpawnLocations] 的出生点（路点序号 + 1 即大厅显示的出生点编号）与 [MultiN_Alliances] 的同盟关系。
    public sealed class ReplayMatchSummary
    {
        private const int MaximumPlayers = 64;
        public string GameMode { get; private set; }
        public int ParticipantCount { get; private set; }
        public int SpectatorCount { get; private set; }
        public int AiCount { get; private set; }
        public List<ReplayPlayerRow> Players { get; private set; }
        // 下拉选项的“名称、取值”，按客户端大厅顺序。
        public List<KeyValuePair<string, string>> Settings { get; private set; }
        public List<string> EnabledSwitches { get; private set; }
        public List<string> DisabledSwitches { get; private set; }

        private sealed class Human
        {
            public string Name;
            public bool IsRecorder;
            public bool IsSpectator;
            public int Side = int.MinValue;
            public int Color = int.MinValue;
            public int Multi; // 1 起；0 表示无法确定
        }

        public static ReplayMatchSummary Parse(byte[] spawnIniBytes, ReplayDisplayCatalog catalog)
        {
            if (catalog == null) catalog = ReplayDisplayCatalog.Empty;
            ReplayIni ini = ReplayIni.Parse(spawnIniBytes);
            ReplayMatchSummary summary = new ReplayMatchSummary
            {
                GameMode = ini.Get("Settings", "UIGameMode"),
                Players = new List<ReplayPlayerRow>(),
                Settings = new List<KeyValuePair<string, string>>(),
                EnabledSwitches = new List<string>(),
                DisabledSwitches = new List<string>()
            };

            // 真人：录制者在 [Settings]，其余按 [Other1..] 顺序。
            List<Human> humans = new List<Human>();
            humans.Add(ReadHuman(ini, "Settings", true));
            for (int n = 1; n <= MaximumPlayers && ini.HasSection("Other" + n.ToString(System.Globalization.CultureInfo.InvariantCulture)); n++)
                humans.Add(ReadHuman(ini, "Other" + n.ToString(System.Globalization.CultureInfo.InvariantCulture), false));
            int declaredHumans;
            if (!ReplayIni.TryInt(ini.Get("Settings", "PlayerCount"), out declaredHumans)) declaredHumans = humans.Count;
            int aiCount;
            if (!ReplayIni.TryInt(ini.Get("Settings", "AIPlayers"), out aiCount) || aiCount < 0) aiCount = 0;
            summary.AiCount = aiCount;
            summary.SpectatorCount = humans.Count(h => h.IsSpectator);
            summary.ParticipantCount = humans.Count - summary.SpectatorCount;

            // MultiN 的真人顺序由颜色序号升序决定；人数与声明不符或颜色重复时无法对应，出生点与同盟不显示具体值。
            bool multiKnown = declaredHumans == humans.Count && humans.All(h => h.Color != int.MinValue) &&
                humans.Select(h => h.Color).Distinct().Count() == humans.Count;
            if (multiKnown)
            {
                int order = 1;
                foreach (Human human in humans.OrderBy(h => h.Color)) human.Multi = order++;
            }

            Dictionary<int, int> allianceGroups = ReadAllianceGroups(ini);
            foreach (Human human in humans)
            {
                ReplayPlayerRow row = new ReplayPlayerRow
                {
                    Name = string.IsNullOrWhiteSpace(human.Name) ? "（未命名）" : human.Name,
                    IsRecorder = human.IsRecorder,
                    IsSpectator = human.IsSpectator,
                    SideText = human.IsSpectator ? "观察者" : FormatSide(catalog, human.Side),
                };
                SetColor(row, catalog, human.Color);
                SetPositionAndAlliance(row, ini, allianceGroups, human.Multi, human.IsSpectator);
                summary.Players.Add(row);
            }

            // 电脑：序号紧接真人之后（与 WriteSpawnIni 一致，偏移取 PlayerCount）。
            for (int i = 0; i < aiCount && i < MaximumPlayers; i++)
            {
                int multi = declaredHumans + i + 1;
                string key = "Multi" + multi.ToString(System.Globalization.CultureInfo.InvariantCulture);
                int level, side, color;
                string levelText = ReplayIni.TryInt(ini.Get("HouseHandicaps", key), out level) ? AiLevelText(level) : null;
                ReplayPlayerRow row = new ReplayPlayerRow
                {
                    Name = levelText == null ? "电脑" : "电脑（" + levelText + "）",
                    IsAi = true,
                    SideText = ReplayIni.TryInt(ini.Get("HouseCountries", key), out side) ? FormatSide(catalog, side) : "未知"
                };
                SetColor(row, catalog, ReplayIni.TryInt(ini.Get("HouseColors", key), out color) ? color : int.MinValue);
                SetPositionAndAlliance(row, ini, allianceGroups, multi, false);
                summary.Players.Add(row);
            }

            foreach (ReplayOptionDefinition option in catalog.Options)
            {
                string value = ini.Get("Settings", option.SpawnKey);
                if (value == null) continue;
                if (option.IsSwitch)
                {
                    bool? enabled = SwitchState(option, value);
                    if (enabled == null) continue;
                    (enabled.Value ? summary.EnabledSwitches : summary.DisabledSwitches).Add(option.Label);
                }
                else
                {
                    string text = DropDownText(option, value);
                    if (text != null) summary.Settings.Add(new KeyValuePair<string, string>(option.Label, text));
                }
            }
            return summary;
        }

        private static Human ReadHuman(ReplayIni ini, string section, bool recorder)
        {
            Human human = new Human { Name = ini.Get(section, "Name"), IsRecorder = recorder };
            human.IsSpectator = ReplayIni.ToBool(ini.Get(section, "IsSpectator"), false);
            int value;
            if (ReplayIni.TryInt(ini.Get(section, "Side"), out value)) human.Side = value;
            if (ReplayIni.TryInt(ini.Get(section, "Color"), out value)) human.Color = value;
            return human;
        }

        private static string FormatSide(ReplayDisplayCatalog catalog, int side)
        {
            if (side == int.MinValue) return "未知";
            return catalog.SideName(side) ?? "阵营 " + side.ToString(System.Globalization.CultureInfo.InvariantCulture);
        }

        // 电脑难度：与客户端大厅 AILevelToName 一致（0 困难、1 中等、2 简单）；其他取值不猜测。
        private static string AiLevelText(int level)
        {
            switch (level)
            {
                case 0: return "困难";
                case 1: return "中等";
                case 2: return "简单";
                default: return "难度 " + level.ToString(System.Globalization.CultureInfo.InvariantCulture);
            }
        }

        private static void SetColor(ReplayPlayerRow row, ReplayDisplayCatalog catalog, int color)
        {
            if (color == int.MinValue) { row.ColorText = "未知"; return; }
            ReplayColorDefinition definition = catalog.FindColor(color);
            if (definition == null)
            {
                row.ColorText = "颜色 " + color.ToString(System.Globalization.CultureInfo.InvariantCulture);
                return;
            }
            row.ColorText = definition.Name;
            row.HasColor = true;
            row.ColorR = definition.R;
            row.ColorG = definition.G;
            row.ColorB = definition.B;
        }

        // [SpawnLocations] MultiN 为路点序号，大厅显示的出生点编号为序号 + 1；未写入表示由游戏随机分配。
        private static void SetPositionAndAlliance(ReplayPlayerRow row, ReplayIni ini, Dictionary<int, int> groups, int multi, bool spectator)
        {
            if (spectator) { row.StartText = "-"; row.AllianceText = "-"; return; }
            if (multi <= 0) { row.StartText = "未知"; row.AllianceText = "未知"; return; }
            string key = "Multi" + multi.ToString(System.Globalization.CultureInfo.InvariantCulture);
            string location = ini.Get("SpawnLocations", key);
            int waypoint;
            if (location == null) row.StartText = "随机";
            else if (ReplayIni.TryInt(location, out waypoint) && waypoint >= 0 && waypoint < 90)
                row.StartText = (waypoint + 1).ToString(System.Globalization.CultureInfo.InvariantCulture);
            else row.StartText = location;
            int group;
            row.AllianceText = groups.TryGetValue(multi, out group) ? "同盟 " + group.ToString(System.Globalization.CultureInfo.InvariantCulture) : "-";
        }

        // 读取 [MultiK_Alliances]（HouseAlly* 的值是盟友 Multi 序号 - 1），把互相结盟的玩家合并成同盟组；
        // 组号按组内最小 Multi 序号升序编为 1、2…（大厅的 A~D 队伍字母不会写入 spawn.ini，因此不显示字母）。
        private static Dictionary<int, int> ReadAllianceGroups(ReplayIni ini)
        {
            Dictionary<int, int> parent = new Dictionary<int, int>();
            Func<int, int> find = null;
            find = x =>
            {
                int p;
                if (!parent.TryGetValue(x, out p)) { parent[x] = x; return x; }
                if (p == x) return x;
                int root = find(p);
                parent[x] = root;
                return root;
            };
            foreach (string section in ini.SectionNames)
            {
                const string Prefix = "Multi";
                const string Suffix = "_Alliances";
                if (!section.StartsWith(Prefix, StringComparison.OrdinalIgnoreCase) ||
                    !section.EndsWith(Suffix, StringComparison.OrdinalIgnoreCase) ||
                    section.Length <= Prefix.Length + Suffix.Length) continue;
                int owner;
                if (!ReplayIni.TryInt(section.Substring(Prefix.Length, section.Length - Prefix.Length - Suffix.Length), out owner) || owner <= 0)
                    continue;
                foreach (KeyValuePair<string, string> entry in ini.Section(section))
                {
                    int ally;
                    if (!entry.Key.StartsWith("HouseAlly", StringComparison.OrdinalIgnoreCase) ||
                        !ReplayIni.TryInt(entry.Value, out ally) || ally < 0 || ally >= MaximumPlayers) continue;
                    int a = find(owner), b = find(ally + 1);
                    if (a != b) parent[Math.Max(a, b)] = Math.Min(a, b);
                }
            }
            Dictionary<int, List<int>> members = new Dictionary<int, List<int>>();
            foreach (int member in parent.Keys.ToList())
            {
                int root = find(member);
                List<int> list;
                if (!members.TryGetValue(root, out list)) members[root] = list = new List<int>();
                list.Add(member);
            }
            Dictionary<int, int> result = new Dictionary<int, int>();
            int number = 1;
            foreach (List<int> group in members.Values.Where(g => g.Count >= 2).OrderBy(g => g.Min()))
            {
                foreach (int member in group) result[member] = number;
                number++;
            }
            return result;
        }

        // 复选框写入 (已勾选 != 反转) ? 启用值 : 禁用值，因此勾选状态 = (写入值 == 启用值) != 反转。两值都不匹配时不确认。
        private static bool? SwitchState(ReplayOptionDefinition option, string value)
        {
            bool isEnabledValue = string.Equals(value, option.EnabledValue, StringComparison.OrdinalIgnoreCase);
            bool isDisabledValue = string.Equals(value, option.DisabledValue, StringComparison.OrdinalIgnoreCase);
            if (isEnabledValue == isDisabledValue) return null;
            return isEnabledValue != option.Reversed;
        }

        private static string DropDownText(ReplayOptionDefinition option, string value)
        {
            if (option.IndexMode)
            {
                int index;
                return ReplayIni.TryInt(value, out index) && index >= 0 && index < option.ItemTexts.Count ? option.ItemTexts[index] : null;
            }
            for (int i = 0; i < option.ItemValues.Count; i++)
                if (string.Equals(option.ItemValues[i], value, StringComparison.Ordinal)) return option.ItemTexts[i];
            return value;
        }
    }
}
