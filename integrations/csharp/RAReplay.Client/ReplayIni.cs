using System;
using System.Collections.Generic;
using System.Text;

namespace RAReplay.Client
{
    internal sealed class ReplayIni
    {
        private readonly Dictionary<string, List<KeyValuePair<string, string>>> sections =
            new Dictionary<string, List<KeyValuePair<string, string>>>(StringComparer.OrdinalIgnoreCase);

        public static ReplayIni Parse(byte[] data)
        {
            ReplayIni ini = new ReplayIni();
            if (data == null) return ini;
            string text = new UTF8Encoding(false, false).GetString(data);
            if (text.Length > 0 && text[0] == '﻿') text = text.Substring(1);
            List<KeyValuePair<string, string>> current = null;
            foreach (string rawLine in text.Split('\n'))
            {
                string line = rawLine;
                int comment = line.IndexOf(';');
                if (comment >= 0) line = line.Substring(0, comment);
                line = line.Trim();
                if (line.Length == 0) continue;
                if (line[0] == '[')
                {
                    int close = line.IndexOf(']');
                    if (close > 0)
                    {
                        string name = line.Substring(1, close - 1).Trim();
                        if (!ini.sections.TryGetValue(name, out current))
                        {
                            current = new List<KeyValuePair<string, string>>();
                            ini.sections[name] = current;
                        }
                    }
                    else current = null;
                    continue;
                }
                int equals = line.IndexOf('=');
                if (current == null || equals <= 0) continue;
                current.Add(new KeyValuePair<string, string>(line.Substring(0, equals).Trim(), line.Substring(equals + 1).Trim()));
            }
            return ini;
        }

        public IEnumerable<string> SectionNames { get { return sections.Keys; } }

        public bool HasSection(string section) { return sections.ContainsKey(section); }

        public IList<KeyValuePair<string, string>> Section(string section)
        {
            List<KeyValuePair<string, string>> entries;
            return sections.TryGetValue(section, out entries) ? (IList<KeyValuePair<string, string>>)entries :
                new List<KeyValuePair<string, string>>();
        }

        // 键不存在返回 null；同名键取最后一个。
        public string Get(string section, string key)
        {
            List<KeyValuePair<string, string>> entries;
            if (section == null || !sections.TryGetValue(section, out entries)) return null;
            for (int i = entries.Count - 1; i >= 0; i--)
                if (string.Equals(entries[i].Key, key, StringComparison.OrdinalIgnoreCase)) return entries[i].Value;
            return null;
        }

        public static bool TryInt(string value, out int result)
        {
            return int.TryParse(value == null ? null : value.Trim(), System.Globalization.NumberStyles.AllowLeadingSign,
                System.Globalization.CultureInfo.InvariantCulture, out result);
        }

        // 与客户端 Conversions.BooleanFromString 一致：yes/true/1 为真，no/false/0 为假，其余取默认值。
        public static bool ToBool(string value, bool defaultValue)
        {
            if (value == null) return defaultValue;
            string text = value.Trim();
            if (text.Equals("yes", StringComparison.OrdinalIgnoreCase) || text.Equals("true", StringComparison.OrdinalIgnoreCase) || text == "1") return true;
            if (text.Equals("no", StringComparison.OrdinalIgnoreCase) || text.Equals("false", StringComparison.OrdinalIgnoreCase) || text == "0") return false;
            return defaultValue;
        }
    }

}
