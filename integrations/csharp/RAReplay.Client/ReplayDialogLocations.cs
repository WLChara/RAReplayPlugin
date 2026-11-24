using System;
using System.IO;
using System.Runtime.InteropServices;

namespace RAReplay.Client
{
    // 文件对话框的初始位置：导入优先“下载”文件夹（已知文件夹），没有就用桌面；导出用桌面。
    public static class ReplayDialogLocations
    {
        public static string ImportStartDirectory()
        {
            return ImportStartDirectory(GetDownloadsDirectory, GetDesktopDirectory);
        }

        public static string ExportStartDirectory()
        {
            return Existing(GetDesktopDirectory);
        }

        internal static string ImportStartDirectory(Func<string> downloads, Func<string> desktop)
        {
            return Existing(downloads) ?? Existing(desktop);
        }

        private static string Existing(Func<string> locate)
        {
            try
            {
                string path = locate();
                return !string.IsNullOrEmpty(path) && Directory.Exists(path) ? path : null;
            }
            catch (Exception) { return null; }
        }

        public static string GetDesktopDirectory()
        {
            return Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        }

        // FOLDERID_Downloads；用户改过下载位置时也能取到。失败返回 null。
        public static string GetDownloadsDirectory()
        {
            Guid downloads = new Guid("374DE290-123F-4565-9164-39C4925E467B");
            IntPtr buffer;
            if (SHGetKnownFolderPath(ref downloads, 0, IntPtr.Zero, out buffer) != 0) return null;
            try { return Marshal.PtrToStringUni(buffer); }
            finally { Marshal.FreeCoTaskMem(buffer); }
        }

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        private static extern int SHGetKnownFolderPath(ref Guid folderId, uint flags, IntPtr token, out IntPtr path);
    }
}
