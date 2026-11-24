using System;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;

namespace RAReplay.Client
{
    // 回放文件的删除：规则判定是纯函数；实际删除只把文件移到回收站，任何一项检查失败都不会碰文件。
    public static class ReplayFileOperations
    {
        // 规范化后必须直接位于回放目录下（不含子目录），文件名以 .yrrep 或 .yrrep.partial 结尾（不区分大小写）且主名非空，
        // 且文件名不含系统不允许的字符（含通配符、冒号/备用数据流）。
        public static bool TryGetDeletablePath(string replaysDirectory, string path, out string fullPath, out string error)
        {
            fullPath = null;
            if (string.IsNullOrWhiteSpace(replaysDirectory) || string.IsNullOrWhiteSpace(path))
            {
                error = "回放路径为空。";
                return false;
            }
            string directory, candidate;
            try
            {
                directory = System.IO.Path.GetFullPath(replaysDirectory).TrimEnd('\\', '/');
                candidate = System.IO.Path.GetFullPath(path);
            }
            catch (Exception)
            {
                error = "回放路径无效。";
                return false;
            }
            string name = System.IO.Path.GetFileName(candidate);
            string parent = System.IO.Path.GetDirectoryName(candidate);
            if (string.IsNullOrEmpty(name) || parent == null ||
                !string.Equals(parent.TrimEnd('\\', '/'), directory, StringComparison.OrdinalIgnoreCase))
            {
                error = "只能删除回放目录下的文件，不能删除子目录或其他位置的文件。";
                return false;
            }
            if (name.IndexOfAny(System.IO.Path.GetInvalidFileNameChars()) >= 0)
            {
                error = "回放文件名包含无效字符。";
                return false;
            }
            int suffix = name.EndsWith(".yrrep.partial", StringComparison.OrdinalIgnoreCase) ? ".yrrep.partial".Length :
                name.EndsWith(".yrrep", StringComparison.OrdinalIgnoreCase) ? ".yrrep".Length : 0;
            if (suffix == 0 || name.Length == suffix)
            {
                error = "只能删除后缀为 .yrrep 或 .yrrep.partial 的回放文件。";
                return false;
            }
            fullPath = candidate;
            error = null;
            return true;
        }

        // 删除前的全部检查；返回 null 表示可以删除，否则为拒绝原因。会读取文件，应在后台线程调用。
        public static string CheckDeletable(string replaysDirectory, ReplayArchive replay, bool replaySessionActive)
        {
            if (replay == null) return "没有选中的回放。";
            if (replaySessionActive) return "回放正在启动或播放，请先退出游戏后再删除回放。";
            string fullPath, error;
            if (!TryGetDeletablePath(replaysDirectory, replay.Path, out fullPath, out error)) return error;
            if (!File.Exists(fullPath)) return "回放文件已不存在，请刷新列表。";
            try
            {
                if ((File.GetAttributes(fullPath) & FileAttributes.ReparsePoint) != 0)
                    return "该文件是符号链接或重分析点，不支持删除。";
                if (!replay.MatchesCurrentHeader()) return "回放文件已被改动，与列表中显示的不一致，请刷新列表后重试。";
            }
            catch (EndOfStreamException ex) { return "无法确认回放文件：" + ex.Message; }
            catch (IOException) { return "回放文件正被占用（可能正在录制或播放），无法删除。"; }
            catch (Exception ex) { return "无法确认回放文件：" + ex.Message; }
            try
            {
                // 正在录制或正在播放的文件被其他进程占用，独占打开会失败。
                using (new FileStream(fullPath, FileMode.Open, FileAccess.Read, FileShare.None)) { }
            }
            catch (IOException) { return "回放文件正被占用（可能正在录制或播放），无法删除。"; }
            catch (UnauthorizedAccessException) { return "没有权限删除该回放文件。"; }
            return null;
        }

        // 检查后把文件移到回收站；返回 null 表示成功，否则为失败原因。
        public static string Delete(string replaysDirectory, ReplayArchive replay, bool replaySessionActive)
        {
            string reason = CheckDeletable(replaysDirectory, replay, replaySessionActive);
            if (reason != null) return reason;
            return MoveToRecycleBin(System.IO.Path.GetFullPath(replay.Path));
        }

        // shell32 SHFileOperationW：FO_DELETE + FOF_ALLOWUNDO 移到回收站，无进度、确认与错误界面。
        // 额外加 FOF_WANTNUKEWARNING：文件无法进回收站（过大、卷已禁用回收站）时由系统提示，而不是静默永久删除。
        // 压缩旧回放（ReplayConverter）也用它处理替换下来的原文件：宿主把它传给 ReplayConversionOptions.MoveToRecycleBin。
        public static string MoveToRecycleBin(string fullPath)
        {
            const uint FO_DELETE = 3;
            const ushort Flags = 0x0004 | 0x0010 | 0x0040 | 0x0400 | 0x4000;
            string from = fullPath + "\0\0"; // 路径列表以双 NUL 结尾
            int result, aborted;
            try
            {
                if (IntPtr.Size == 4)
                {
                    ShFileOpStruct32 operation = new ShFileOpStruct32 { wFunc = FO_DELETE, pFrom = from, fFlags = Flags };
                    result = SHFileOperation32(ref operation);
                    aborted = operation.fAnyOperationsAborted;
                }
                else
                {
                    ShFileOpStruct64 operation = new ShFileOpStruct64 { wFunc = FO_DELETE, pFrom = from, fFlags = Flags };
                    result = SHFileOperation64(ref operation);
                    aborted = operation.fAnyOperationsAborted;
                }
            }
            catch (Exception ex) { return "无法调用系统回收站：" + ex.Message; }
            if (aborted != 0) return "删除已被取消，文件未移动。";
            if (result != 0) return "移到回收站失败（系统错误代码 0x" + result.ToString("X", CultureInfo.InvariantCulture) + "），文件未删除。";
            if (File.Exists(fullPath)) return "文件仍然存在，未能移到回收站。";
            return null;
        }

        // SHFILEOPSTRUCTW：32 位按 1 字节对齐，64 位按自然对齐。
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode, Pack = 1)]
        private struct ShFileOpStruct32
        {
            public IntPtr hwnd;
            public uint wFunc;
            public string pFrom;
            public string pTo;
            public ushort fFlags;
            public int fAnyOperationsAborted;
            public IntPtr hNameMappings;
            public string lpszProgressTitle;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct ShFileOpStruct64
        {
            public IntPtr hwnd;
            public uint wFunc;
            public string pFrom;
            public string pTo;
            public ushort fFlags;
            public int fAnyOperationsAborted;
            public IntPtr hNameMappings;
            public string lpszProgressTitle;
        }

        [DllImport("shell32.dll", CharSet = CharSet.Unicode, EntryPoint = "SHFileOperationW")]
        private static extern int SHFileOperation32(ref ShFileOpStruct32 operation);

        [DllImport("shell32.dll", CharSet = CharSet.Unicode, EntryPoint = "SHFileOperationW")]
        private static extern int SHFileOperation64(ref ShFileOpStruct64 operation);
    }
}
