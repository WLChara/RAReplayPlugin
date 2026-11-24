using System;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

namespace RAReplay.Client
{
    // ===== 导出 =====
    // 把列表中选中的回放原样另存为文件，方便发给别人；内容与源文件逐字节相同（未完成录像也不改头部）。
    // Export 先写到目标目录的临时文件并核对大小与 SHA-256，再放到最终位置；失败时清理临时文件，源文件任何情况下都不会被修改。
    public static class ReplayExporter
    {
        internal const string InsideReplaysText = "不能导出到回放文件夹里，请选择桌面或其他位置";
        internal const string SameAsSourceText = "不能导出到这个回放文件本身，请换一个位置或文件名";
        internal const string ChangedSourceText = "回放文件已被改动，与列表中显示的不一致，请刷新列表后重试";
        internal const string BusySourceText = "回放文件正被占用（可能正在录制或播放），暂时无法导出";
        internal const string VerifyFailedText = "导出的文件与原回放不一致，可能是磁盘出了问题，已取消导出";
        private const int BufferBytes = 80 * 1024;
        private const int MaximumMapNameChars = 60;
        private const uint MOVEFILE_REPLACE_EXISTING = 0x1;

        // 默认文件名：<地图名> <yyyy-MM-dd HH-mm>.yrrep；未完成录像在扩展名前加“（未完成）”。
        public static string DefaultFileName(string mapName, DateTime recordedLocalTime, bool unfinalized)
        {
            string map = ReplayImporter.SanitizeFileNamePart(mapName, MaximumMapNameChars);
            if (map.Length == 0) map = "回放";
            return map + " " + recordedLocalTime.ToString("yyyy-MM-dd HH-mm", CultureInfo.InvariantCulture) +
                (unfinalized ? "（未完成）" : string.Empty) + ".yrrep";
        }

        // explorer.exe 选中并定位文件的参数；路径整体加引号（路径里可含空格与逗号）。
        public static string ExplorerSelectArguments(string path)
        {
            return "/select,\"" + path + "\"";
        }

        // 目标路径检查（纯路径运算，不读写文件）。返回 null 表示可以导出，否则为拒绝原因。
        // 拒绝：与源文件相同；位于回放目录及其子目录内。
        public static string CheckDestination(string replaysDirectory, string sourcePath, string destination, out string fullDestination)
        {
            fullDestination = null;
            if (string.IsNullOrWhiteSpace(destination)) return "没有选择保存位置";
            if (string.IsNullOrWhiteSpace(replaysDirectory)) return "回放文件夹的位置未知，无法导出";
            string full, source, replays;
            try
            {
                full = Path.GetFullPath(destination);
                source = Path.GetFullPath(sourcePath);
                replays = Path.GetFullPath(replaysDirectory).TrimEnd('\\', '/');
            }
            catch (Exception) { return "保存位置无效，请换一个位置"; }
            string name = ReplayImporter.LastSegment(full);
            if (name.Length == 0 || name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0) return "文件名无效，请换一个文件名";
            if (string.Equals(full, source, StringComparison.OrdinalIgnoreCase)) return SameAsSourceText;
            if (full.StartsWith(replays + "\\", StringComparison.OrdinalIgnoreCase) ||
                full.StartsWith(replays + "/", StringComparison.OrdinalIgnoreCase)) return InsideReplaysText;
            fullDestination = full;
            return null;
        }

        // 导出并返回 null 表示成功，否则为给玩家看的原因（detail 为技术细节，可能为 null）。会读写大文件，应在后台线程调用。
        public static string Export(string replaysDirectory, ReplayArchive replay, string destination, out string detail)
        {
            return Export(replaysDirectory, replay, destination, out detail, null, null);
        }

        // wrapOutput / wrapVerify 仅供测试注入写入与核对阶段的故障；正式调用走上面的重载，不传。
        internal static string Export(string replaysDirectory, ReplayArchive replay, string destination, out string detail,
            Func<Stream, Stream> wrapOutput, Func<Stream, Stream> wrapVerify)
        {
            detail = null;
            if (replay == null) return "没有选中的回放";
            string fullDestination;
            string rejected = CheckDestination(replaysDirectory, replay.Path, destination, out fullDestination);
            if (rejected != null) return rejected;
            string directory = Path.GetDirectoryName(fullDestination);
            if (string.IsNullOrEmpty(directory) || !Directory.Exists(directory)) return "保存位置所在的文件夹不存在，请换一个位置";
            if (Directory.Exists(fullDestination)) return "这个位置已经有一个同名的文件夹，请换一个文件名";

            // 源文件必须仍是列表上显示的那一个。
            try
            {
                if (!replay.MatchesCurrentHeader()) return ChangedSourceText;
            }
            catch (FileNotFoundException) { return "回放文件已不存在，请刷新列表"; }
            catch (DirectoryNotFoundException) { return "回放文件已不存在，请刷新列表"; }
            catch (IOException ex)
            {
                detail = ex.ToString();
                return BusySourceText;
            }
            catch (Exception ex)
            {
                detail = ex.ToString();
                return "无法确认回放文件，请刷新列表后重试";
            }

            string temp = Path.Combine(directory, Guid.NewGuid().ToString("N") + ".exporting");
            try
            {
                long sourceLength;
                byte[] sourceHash;
                CopyAndHash(replay.Path, temp, wrapOutput, out sourceLength, out sourceHash);
                long writtenLength;
                byte[] writtenHash = HashFile(temp, wrapVerify, out writtenLength);
                if (writtenLength != sourceLength || !ReplayLaunchManifest.EqualsHash(writtenHash, sourceHash))
                {
                    detail = "临时文件 " + writtenLength + " 字节与源文件 " + sourceLength + " 字节或其 SHA-256 不一致。";
                    return VerifyFailedText;
                }
                // 核对通过后才放到最终位置；目标已存在（用户已在保存窗口确认覆盖）时用原子替换。
                if (File.Exists(fullDestination)) ReplaceExisting(temp, fullDestination);
                else File.Move(temp, fullDestination);
                return null;
            }
            catch (Exception ex)
            {
                detail = ex.ToString();
                return DescribeFailure(ex);
            }
            finally
            {
                try { if (File.Exists(temp)) File.Delete(temp); }
                catch (Exception) { /* 临时文件无法删除时，用户可手动清理 .exporting 文件。 */ }
            }
        }

        private static string DescribeFailure(Exception ex)
        {
            if (ex is UnauthorizedAccessException) return "没有权限写入这个位置，请换一个位置（例如桌面）";
            if (ex is PathTooLongException) return "保存路径太长，请换一个位置或更短的文件名";
            IOException io = ex as IOException;
            if (io != null)
            {
                int code = io.HResult & 0xFFFF; // ERROR_HANDLE_DISK_FULL=39，ERROR_DISK_FULL=112
                if (code == 112 || code == 39) return "磁盘空间不足，无法导出";
                return "写入时出错，可能磁盘已满、U 盘已拔出，或同名文件正被其他程序占用";
            }
            return "导出时出现意外错误";
        }

        // 以只读共享方式读取源文件，写入目标临时文件，同时计算源数据的 SHA-256。
        private static void CopyAndHash(string sourcePath, string tempPath, Func<Stream, Stream> wrapOutput,
            out long length, out byte[] hash)
        {
            using (FileStream source = new FileStream(sourcePath, FileMode.Open, FileAccess.Read, FileShare.Read,
                BufferBytes, FileOptions.SequentialScan))
            using (FileStream raw = new FileStream(tempPath, FileMode.CreateNew, FileAccess.Write, FileShare.None, BufferBytes))
            using (SHA256 sha = SHA256.Create())
            {
                Stream output = wrapOutput != null ? wrapOutput(raw) : raw;
                byte[] buffer = new byte[BufferBytes];
                long copied = 0;
                int read;
                while ((read = source.Read(buffer, 0, buffer.Length)) > 0)
                {
                    sha.TransformBlock(buffer, 0, read, null, 0);
                    output.Write(buffer, 0, read);
                    copied += read;
                }
                sha.TransformFinalBlock(new byte[0], 0, 0);
                output.Flush();
                raw.Flush(true);
                if (copied != source.Length) throw new IOException("复制过程中源文件大小发生了变化。");
                length = copied;
                hash = sha.Hash;
            }
        }

        private static byte[] HashFile(string path, Func<Stream, Stream> wrapInput, out long length)
        {
            using (FileStream raw = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read,
                BufferBytes, FileOptions.SequentialScan))
            using (SHA256 sha = SHA256.Create())
            {
                Stream input = wrapInput != null ? wrapInput(raw) : raw;
                byte[] buffer = new byte[BufferBytes];
                long total = 0;
                int read;
                while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
                {
                    sha.TransformBlock(buffer, 0, read, null, 0);
                    total += read;
                }
                sha.TransformFinalBlock(new byte[0], 0, 0);
                length = total;
                return sha.Hash;
            }
        }

        // 同卷改名并替换已有文件（MoveFileEx + REPLACE_EXISTING）：NTFS 上是原子的，FAT/exFAT（U 盘）上同样可用。
        internal static void ReplaceExisting(string tempPath, string destination)
        {
            if (!MoveFileEx(tempPath, destination, MOVEFILE_REPLACE_EXISTING))
            {
                int error = Marshal.GetLastWin32Error();
                throw new IOException("无法替换已有文件：" + destination, new Win32Exception(error));
            }
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "MoveFileExW")]
        private static extern bool MoveFileEx(string existingFileName, string newFileName, uint flags);
    }
}
