using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace RAReplay.Client
{
    // ===== 导入 =====
    // 玩家收到的回放可能是 .yrrep / .yrrep.partial，可能被聊天软件改过名，也可能装在 .zip 压缩包里。
    // 这里只按文件内容识别，把能在本机播放的回放放进回放文件夹，玩家只需“选文件”。
    // 读写文件的方法都应在非界面线程调用。用户选中的源文件和压缩包始终只读，任何情况下都不会被修改或删除。
    // Reason 是给玩家看的通俗简体中文；技术细节只放在 Detail，供日志使用。

    public enum ReplayContentKind { Unknown, Replay, Zip, Rar, SevenZip }

    // 导入上限，可注入以便测试；默认值见 Default。
    public sealed class ReplayImportLimits
    {
        // 单个回放（压缩包条目或独立文件）的字节上限：容器头 96 + 清单 16 MiB + 帧流 1 GiB。
        public long MaxEntryBytes { get; set; }
        // 单个压缩包解出的回放合计字节上限。
        public long MaxTotalBytes { get; set; }
        // 单个压缩包内可导入的回放条目数上限。
        public int MaxReplayEntries { get; set; }
        // 单个压缩包最多检查的条目数：识别每个条目都要读取其开头，条目过多会拖慢导入。
        public int MaxScannedEntries { get; set; }

        public static ReplayImportLimits Default
        {
            get
            {
                return new ReplayImportLimits
                {
                    MaxEntryBytes = 96L + 16L * 1024 * 1024 + 1024L * 1024 * 1024,
                    MaxTotalBytes = 4L * 1024 * 1024 * 1024,
                    MaxReplayEntries = 200,
                    MaxScannedEntries = 10000
                };
            }
        }
    }

    public sealed class ReplayImportOptions
    {
        public string ReplaysDirectory { get; set; }
        // 本机游戏主程序的 SHA-256；为 null 表示读取失败，此时拒绝导入（无法确认版本）。
        public byte[] LocalGameExeHash { get; set; }
        public ReplayImportLimits Limits { get; set; }
        // 打开压缩包的方法；为 null 时压缩包按“暂不支持”处理。
        public Func<string, IReplayZipArchive> OpenZip { get; set; }
        public Func<DateTime> Now { get; set; }

        internal ReplayImportLimits EffectiveLimits { get { return Limits ?? ReplayImportLimits.Default; } }
        internal DateTime CurrentTime { get { return Now != null ? Now() : DateTime.Now; } }
    }

    public sealed class ReplayImportSkip
    {
        // 玩家看得懂的来源名：文件名，或“压缩包名 中的 文件名”。
        public string Source { get; set; }
        public string Reason { get; set; }
        public string Detail { get; set; }
    }

    public sealed class ReplayImportResult
    {
        private readonly List<string> imported = new List<string>();
        private readonly List<ReplayImportSkip> skipped = new List<ReplayImportSkip>();

        // 新放入回放文件夹的文件完整路径，按导入顺序。
        public IList<string> ImportedPaths { get { return imported; } }
        public IList<ReplayImportSkip> Skipped { get { return skipped; } }
        public string FirstImportedPath { get { return imported.Count > 0 ? imported[0] : null; } }

        // 只含一条失败记录的结果：宿主在导入流程之外出现意外错误时，用它沿同一条提示路径向玩家说明。
        public static ReplayImportResult FromFailure(string source, string reason, string detail)
        {
            ReplayImportResult result = new ReplayImportResult();
            result.Skip(source, reason, detail);
            return result;
        }

        internal void AddImported(string path) { imported.Add(path); }

        internal void Skip(string source, string reason, string detail)
        {
            skipped.Add(new ReplayImportSkip { Source = source, Reason = reason, Detail = detail });
        }

        // 丢弃从 index 起记录的跳过项（压缩包整体中止时，不再逐条提示其中已经记下的条目）。
        internal void RemoveSkippedFrom(int index)
        {
            if (index >= 0 && index < skipped.Count) skipped.RemoveRange(index, skipped.Count - index);
        }

        // 汇总文字：先说成功数量，再列出没导入的项和原因（最多 maxSkippedLines 行，其余写“等 K 个”）。
        public string BuildSummary(int maxSkippedLines)
        {
            string newLine = Environment.NewLine;
            StringBuilder text = new StringBuilder();
            text.Append(imported.Count > 0 ?
                "成功导入 " + imported.Count.ToString(CultureInfo.InvariantCulture) + " 个回放。" :
                "没有导入任何回放。");
            if (skipped.Count == 0) return text.ToString();
            text.Append(newLine).Append(newLine);
            text.Append(imported.Count > 0 ?
                "以下 " + skipped.Count.ToString(CultureInfo.InvariantCulture) + " 项没有导入：" : "原因：");
            int shown = Math.Min(skipped.Count, Math.Max(0, maxSkippedLines));
            for (int i = 0; i < shown; i++)
                text.Append(newLine).Append("- ").Append(Shorten(skipped[i].Source, 40)).Append("：").Append(skipped[i].Reason);
            if (skipped.Count > shown)
                text.Append(newLine).Append("等 ").Append((skipped.Count - shown).ToString(CultureInfo.InvariantCulture)).Append(" 个（未全部列出）");
            return text.ToString();
        }

        // 过长的名字取头尾、中间用省略号，避免弹窗被撑得过宽。
        internal static string Shorten(string text, int maxChars)
        {
            if (string.IsNullOrEmpty(text) || text.Length <= maxChars) return text ?? string.Empty;
            int head = (maxChars - 1) / 2;
            int tail = maxChars - 1 - head;
            if (char.IsHighSurrogate(text[head - 1])) head--;
            if (char.IsLowSurrogate(text[text.Length - tail])) tail--;
            return text.Substring(0, head) + "…" + text.Substring(text.Length - tail);
        }
    }
}
