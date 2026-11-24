using System;
using System.Collections.Generic;

namespace RAReplay.Client
{
    public sealed class ReplayColorDefinition
    {
        public string Name { get; set; }
        public byte R { get; set; }
        public byte G { get; set; }
        public byte B { get; set; }
        public int GameColorIndex { get; set; }
    }

    // 大厅选项的展示定义：下拉框（写入选中项序号或选中项取值）或复选框（写入启用/禁用值，可反转）。
    public sealed class ReplayOptionDefinition
    {
        public string SpawnKey { get; set; }
        public string Label { get; set; }
        public bool IsSwitch { get; set; }
        public bool IndexMode { get; set; }
        public List<string> ItemValues { get; set; }
        public List<string> ItemTexts { get; set; }
        public string EnabledValue { get; set; }
        public string DisabledValue { get; set; }
        public bool Reversed { get; set; }
    }

    // 回放列表展示用的客户端定义，来源是客户端自己读取的 Resources/GameOptions.ini：
    // [General] Sides / InternalSideIndices / SpectatorInternalSideIndex、[MPColors]、
    // [MultiplayerGameLobby] / [SkirmishLobby] 中列出的下拉框与复选框（解析方式与 GameLobbyDropDown / GameLobbyCheckBox 一致）。
    public sealed class ReplayDisplayCatalog
    {
        public static readonly ReplayDisplayCatalog Empty = new ReplayDisplayCatalog();

        private readonly List<string> sides = new List<string>();
        private List<int> internalSideIndices;
        private readonly List<ReplayColorDefinition> colors = new List<ReplayColorDefinition>();
        private readonly List<ReplayOptionDefinition> options = new List<ReplayOptionDefinition>();

        public IList<ReplayOptionDefinition> Options { get { return options; } }
        public IList<string> Sides { get { return sides; } }
        public IList<ReplayColorDefinition> Colors { get { return colors; } }

        public static ReplayDisplayCatalog Parse(byte[] gameOptionsIni)
        {
            ReplayIni ini = ReplayIni.Parse(gameOptionsIni);
            ReplayDisplayCatalog catalog = new ReplayDisplayCatalog();
            string sideText = ini.Get("General", "Sides");
            if (!string.IsNullOrEmpty(sideText))
                foreach (string side in sideText.Split(',')) catalog.sides.Add(side.Trim());
            string internalText = ini.Get("General", "InternalSideIndices");
            if (!string.IsNullOrEmpty(internalText))
            {
                List<int> indices = new List<int>();
                foreach (string part in internalText.Split(','))
                {
                    int value;
                    if (!ReplayIni.TryInt(part, out value)) { indices = null; break; }
                    indices.Add(value);
                }
                catalog.internalSideIndices = indices;
            }
            foreach (KeyValuePair<string, string> entry in ini.Section("MPColors"))
            {
                string[] parts = entry.Value.Split(',');
                int r, g, b, index;
                if (parts.Length < 4 || !ReplayIni.TryInt(parts[0], out r) || !ReplayIni.TryInt(parts[1], out g) ||
                    !ReplayIni.TryInt(parts[2], out b) || !ReplayIni.TryInt(parts[3], out index)) continue;
                catalog.colors.Add(new ReplayColorDefinition
                {
                    Name = entry.Key,
                    R = (byte)Math.Max(0, Math.Min(255, r)),
                    G = (byte)Math.Max(0, Math.Min(255, g)),
                    B = (byte)Math.Max(0, Math.Min(255, b)),
                    GameColorIndex = index
                });
            }
            foreach (string lobby in new[] { "MultiplayerGameLobby", "SkirmishLobby" })
            {
                foreach (string name in SplitNames(ini.Get(lobby, "DropDowns"))) catalog.AddDropDown(ini, name);
                foreach (string name in SplitNames(ini.Get(lobby, "CheckBoxes"))) catalog.AddSwitch(ini, name);
            }
            return catalog;
        }

        // 阵营名：spawn.ini 的 Side 是内部阵营序号。配置了 InternalSideIndices 时按其反查客户端阵营序号，否则两者相同。
        public string SideName(int internalSideIndex)
        {
            int sideIndex = internalSideIndex;
            if (internalSideIndices != null && internalSideIndices.Count > 0)
                sideIndex = internalSideIndices.IndexOf(internalSideIndex);
            return sideIndex >= 0 && sideIndex < sides.Count && sides[sideIndex].Length > 0 ? sides[sideIndex] : null;
        }

        public ReplayColorDefinition FindColor(int gameColorIndex)
        {
            foreach (ReplayColorDefinition color in colors)
                if (color.GameColorIndex == gameColorIndex) return color;
            return null;
        }

        private bool HasOption(string spawnKey)
        {
            foreach (ReplayOptionDefinition option in options)
                if (string.Equals(option.SpawnKey, spawnKey, StringComparison.OrdinalIgnoreCase)) return true;
            return false;
        }

        private static IEnumerable<string> SplitNames(string text)
        {
            if (string.IsNullOrEmpty(text)) yield break;
            foreach (string part in text.Split(','))
            {
                string name = part.Trim();
                if (name.Length > 0) yield return name;
            }
        }

        // 仅收录写入 spawn.ini 且名称可由配置确认的下拉框；MapCode/Boolean 写入方式或没有对应标签的下拉框不收录。
        private void AddDropDown(ReplayIni ini, string name)
        {
            string key = ini.Get(name, "SpawnIniOption");
            if (string.IsNullOrEmpty(key) || HasOption(key)) return;
            string mode = (ini.Get(name, "DataWriteMode") ?? string.Empty).Trim().ToUpperInvariant();
            if (mode == "MAPCODE" || mode == "BOOLEAN") return;
            string labelSection = name.StartsWith("cmb", StringComparison.OrdinalIgnoreCase) ? "lbl" + name.Substring(3) : null;
            string label = ini.Get(labelSection, "Text");
            string itemsText = ini.Get(name, "Items");
            if (string.IsNullOrEmpty(label) || string.IsNullOrEmpty(itemsText)) return;
            string[] items = itemsText.Split(',');
            string[] labels = (ini.Get(name, "ItemLabels") ?? string.Empty).Split(',');
            ReplayOptionDefinition option = new ReplayOptionDefinition
            {
                SpawnKey = key,
                Label = label,
                IndexMode = mode == "INDEX",
                ItemValues = new List<string>(),
                ItemTexts = new List<string>()
            };
            for (int i = 0; i < items.Length; i++)
            {
                bool labelled = labels.Length > i && !string.IsNullOrEmpty(labels[i]);
                option.ItemTexts.Add(labelled ? labels[i] : items[i]);
                option.ItemValues.Add(items[i]);
            }
            options.Add(option);
        }

        private void AddSwitch(ReplayIni ini, string name)
        {
            string key = ini.Get(name, "SpawnIniOption");
            string label = ini.Get(name, "Text");
            if (string.IsNullOrEmpty(key) || string.IsNullOrEmpty(label) || HasOption(key)) return;
            string visible = ini.Get(name, "Visible");
            if (visible != null && !ReplayIni.ToBool(visible, true)) return;
            options.Add(new ReplayOptionDefinition
            {
                SpawnKey = key,
                Label = label,
                IsSwitch = true,
                EnabledValue = ini.Get(name, "EnabledSpawnIniValue") ?? "True",
                DisabledValue = ini.Get(name, "DisabledSpawnIniValue") ?? "False",
                Reversed = ReplayIni.ToBool(ini.Get(name, "Reversed"), false)
            });
        }
    }

}
