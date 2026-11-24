using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;

namespace RAReplay.Client
{
    public sealed class ReplayInfoLine
    {
        public string Text { get; set; }
        public bool IsHeader { get; set; }
    }

    // 选中回放后计算的展示数据；文本均为可直接显示的简体中文。
    // Load 会读取文件（帧数统计、文件大小），应在后台线程调用；Compose 只做文本组装。
    public sealed class ReplayDetails
    {
        public List<ReplayInfoLine> Lines { get; private set; }
        public List<ReplayPlayerRow> Players { get; private set; }

        public static ReplayDetails Load(ReplayArchive archive, ReplayDisplayCatalog catalog)
        {
            long fileBytes = -1;
            string frames;
            try { fileBytes = new FileInfo(archive.Path).Length; }
            catch (Exception) { /* 大小不可用时显示“未知大小”。 */ }
            try
            {
                uint? count = archive.CountFrames();
                frames = count.HasValue ? count.Value.ToString(CultureInfo.InvariantCulture) : "无法统计（帧流未通过检查）";
            }
            catch (Exception ex) { frames = "无法统计（" + ex.Message + "）"; }
            ReplayMatchSummary summary = null;
            string summaryError = null;
            try { summary = ReplayMatchSummary.Parse(archive.Manifest.SpawnIniBytes, catalog); }
            catch (Exception ex) { summaryError = ex.Message; }
            return Compose(archive, summary, summaryError, frames, fileBytes);
        }

        public static ReplayDetails Compose(ReplayArchive archive, ReplayMatchSummary summary, string summaryError,
            string frames, long fileBytes)
        {
            ReplayDetails details = new ReplayDetails
            {
                Lines = new List<ReplayInfoLine>(),
                Players = summary == null ? new List<ReplayPlayerRow>() : summary.Players
            };
            DateTime time = DateTimeOffset.FromUnixTimeSeconds(archive.Manifest.RecordedAtUnixSeconds).LocalDateTime;
            details.Header("基本");
            details.Line("地图：" + archive.Manifest.MapName);
            details.Line("录制时间：" + time.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture));
            details.Line(archive.IsUnfinalized ?
                "状态：未完成录像：可恢复 " + archive.RecoveredFrameCount.ToString(CultureInfo.InvariantCulture) + " 帧，未做完整性校验" :
                "状态：完整录像");
            details.Header("对局");
            if (summary == null)
                details.Line("对局信息：无法读取" + (string.IsNullOrEmpty(summaryError) ? string.Empty : "（" + summaryError + "）"));
            else
            {
                if (!string.IsNullOrWhiteSpace(summary.GameMode)) details.Line("游戏模式：" + summary.GameMode);
                details.Line("人数：参战 " + summary.ParticipantCount.ToString(CultureInfo.InvariantCulture) + " 人，观察者 " +
                    summary.SpectatorCount.ToString(CultureInfo.InvariantCulture) + " 人，电脑 " +
                    summary.AiCount.ToString(CultureInfo.InvariantCulture) + " 个");
                if (summary.Settings.Count > 0)
                    details.Line("选项：" + string.Join("，", summary.Settings.Select(setting => setting.Key + " " + setting.Value)));
                if (summary.EnabledSwitches.Count > 0) details.Line("已开启：" + string.Join("、", summary.EnabledSwitches));
                if (summary.DisabledSwitches.Count > 0) details.Line("已关闭：" + string.Join("、", summary.DisabledSwitches));
            }
            details.Header("录像");
            details.Line("帧数：" + frames);
            details.Line("文件：" + System.IO.Path.GetFileName(archive.Path) + "（" + FormatBytes(fileBytes) + "）");
            details.Line("帧流版本：" + archive.StreamVersion.ToString(CultureInfo.InvariantCulture) + "（" + VersionText(archive) + "）");
            return details;
        }

        private void Header(string text) { Lines.Add(new ReplayInfoLine { Text = text, IsHeader = true }); }

        private void Line(string text) { Lines.Add(new ReplayInfoLine { Text = text }); }

        private static string VersionText(ReplayArchive archive)
        {
            if (!archive.HasPlayerInputEffects) return "旧格式，缺少玩家输入效果，无法播放，请重新录制";
            // 版本 5 的单机帧流在录制时已排除飞机绘制对模拟的影响；版本 5 的联机录像与版本 4 文本相同。
            if (archive.StreamVersion >= 5 && archive.StreamGameMode.HasValue &&
                ReplayArchive.IsSinglePlayerMode(archive.StreamGameMode.Value))
                return "含玩家输入效果、编队与游戏速度记录，录制时已排除飞机绘制对模拟的影响";
            if (archive.HasGameSpeedRecords) return "含玩家输入效果、编队与游戏速度记录";
            return archive.HasRecordedGroups ? "含玩家输入效果与编队记录" : "含玩家输入效果，无编队记录";
        }

        // 公开给宿主：压缩旧回放的确认框用它显示候选回放的总大小；转换汇总（ReplayConversionResult）也用这一份。
        public static string FormatBytes(long bytes)
        {
            if (bytes < 0) return "未知大小";
            if (bytes >= 1024 * 1024) return (bytes / 1048576.0).ToString("0.0", CultureInfo.InvariantCulture) + " MB";
            if (bytes >= 1024) return (bytes / 1024.0).ToString("0.0", CultureInfo.InvariantCulture) + " KB";
            return bytes.ToString(CultureInfo.InvariantCulture) + " 字节";
        }
    }
}
