using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace RAReplay.Client
{
    public static class ReplayImporter
    {
        // 玩家可见的提示文字。
        internal const string NotReplayText = "这不是回放文件";
        internal const string UnsupportedArchiveText = "暂不支持这种压缩包，请让对方直接发送回放文件或 .zip 压缩包";
        internal const string VersionMismatchText = "这个回放是用不同版本的游戏录制的，无法在本机播放";
        internal const string LegacyFormatText = "旧格式缺少玩家输入效果，请重新录制";
        internal const string CorruptedReplayText = "回放文件已损坏或不完整，无法使用";
        internal const string GameUnreadableText = "无法读取本机的游戏程序，没法确认这个回放能否播放，请检查游戏文件是否完整";
        internal const string TooLargeText = "文件太大，不是有效的回放";
        internal const string UnreadableSourceText = "无法读取这个文件，可能正被其他程序占用，或已损坏";
        internal const string EncryptedZipText = "压缩包设置了密码，无法导入，请让对方发送没有密码的压缩包，或直接发送回放文件";
        internal const string CorruptedZipText = "压缩包已损坏或不完整，无法读取";
        internal const string CorruptedEntryText = "压缩包里的这个文件已损坏，无法读取";
        internal const string NoReplayInZipText = "压缩包里没有找到回放文件";
        internal const string ZipLimitText = "压缩包里的内容异常庞大或数量异常多，为保护电脑已停止导入";
        internal const string FolderUnavailableText = "无法创建或使用回放文件夹，请检查磁盘空间和文件夹权限";

        private const int HeadBytes = 8;
        private const int BufferBytes = 80 * 1024;
        private const int MaximumBaseNameChars = 80;
        private const int PathLimit = 259;
        // Unicode 替换字符 U+FFFD：解码器遇到无效字节时的输出。
        private const char ReplacementChar = (char)0xFFFD;
        private const string TemporarySuffix = ".importing";
        private static readonly byte[] ReplayMagic = Encoding.ASCII.GetBytes("RAREPL2\0");
        private static readonly byte[] ZipMagic = { 0x50, 0x4B, 0x03, 0x04 };
        private static readonly byte[] EmptyZipMagic = { 0x50, 0x4B, 0x05, 0x06 };
        private static readonly byte[] RarMagic = { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07 };
        private static readonly byte[] SevenZipMagic = { 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C };
        private static readonly string[] KnownSuffixes = { ".yrrep.partial", ".yrrep", ".partial", ".zip", TemporarySuffix };
        private static readonly string[] ReservedDeviceNames =
        {
            "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$",
            "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
            "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
        };

        // 压缩包的体量或数量超限：中止整个压缩包。
        private sealed class LimitException : Exception
        {
            public LimitException(string message) : base(message) { }
        }

        // 读取源数据（独立文件或压缩包条目）时出错，与写入目标时出错区分开。
        private sealed class SourceReadException : Exception
        {
            public SourceReadException(Exception inner) : base(inner.Message, inner) { }
        }

        // 已通过校验、尚未落地的回放临时文件。
        private sealed class PendingReplay
        {
            public string TempPath;
            public ReplayArchive Archive;
            public string DisplayName;
            public string OriginalName;
        }

        // ----- 本机游戏版本 -----

        // 计算本机游戏主程序的 SHA-256，用作 ReplayImportOptions.LocalGameExeHash；任何读取失败都返回 null
        // （导入逻辑据此提示“无法读取本机的游戏程序”，而不是猜测版本）。会读取整个文件，应在后台线程调用。
        public static byte[] TryComputeFileHash(string path)
        {
            try { return ReplayLaunchManifest.HashFile(path); }
            catch (Exception) { return null; }
        }

        // ----- 内容识别 -----

        // 只看开头的魔数，不看扩展名。head 的有效长度为 length。
        public static ReplayContentKind Classify(byte[] head, int length)
        {
            if (head == null) return ReplayContentKind.Unknown;
            if (StartsWith(head, length, ReplayMagic)) return ReplayContentKind.Replay;
            if (StartsWith(head, length, ZipMagic) || StartsWith(head, length, EmptyZipMagic)) return ReplayContentKind.Zip;
            if (StartsWith(head, length, RarMagic)) return ReplayContentKind.Rar;
            if (StartsWith(head, length, SevenZipMagic)) return ReplayContentKind.SevenZip;
            return ReplayContentKind.Unknown;
        }

        public static ReplayContentKind ClassifyFile(string path)
        {
            using (FileStream stream = OpenSource(path))
            {
                byte[] head = new byte[HeadBytes];
                int length = ReadHead(stream, head);
                return Classify(head, length);
            }
        }

        private static bool StartsWith(byte[] data, int length, byte[] magic)
        {
            if (length < magic.Length || data.Length < magic.Length) return false;
            for (int i = 0; i < magic.Length; i++)
                if (data[i] != magic[i]) return false;
            return true;
        }

        private static int ReadHead(Stream stream, byte[] head)
        {
            int total = 0;
            while (total < head.Length)
            {
                int read = stream.Read(head, total, head.Length - total);
                if (read <= 0) break;
                total += read;
            }
            return total;
        }

        // 源文件允许与其他程序共享读取（聊天软件可能仍持有句柄），但我们自己绝不写入。
        private static FileStream OpenSource(string path)
        {
            return new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete,
                BufferBytes, FileOptions.SequentialScan);
        }

        // ----- 命名 -----

        // 取最后一段文件名；压缩包里的条目名可能含任意字符，所以不用会对非法字符抛异常的 Path.GetFileName。
        internal static string LastSegment(string name)
        {
            if (string.IsNullOrEmpty(name)) return string.Empty;
            int cut = Math.Max(name.LastIndexOf('/'), name.LastIndexOf('\\'));
            return cut >= 0 ? name.Substring(cut + 1) : name;
        }

        // 去掉已知后缀（.yrrep.partial、.yrrep、.partial、.zip、.importing，不区分大小写，可叠加）。
        internal static string StripKnownSuffixes(string name)
        {
            string current = name ?? string.Empty;
            bool changed = true;
            while (changed && current.Length > 0)
            {
                changed = false;
                foreach (string suffix in KnownSuffixes)
                {
                    if (current.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
                    {
                        current = current.Substring(0, current.Length - suffix.Length);
                        changed = true;
                        break;
                    }
                }
            }
            return current;
        }

        // 清理成可作文件名的一段：非法字符、控制字符与格式字符换成下划线，去掉首尾空格和点，限长（不拆开代理对）。
        // 清理后没有任何有意义内容时返回空串。
        internal static string SanitizeFileNamePart(string text, int maxChars)
        {
            if (string.IsNullOrEmpty(text) || maxChars <= 0) return string.Empty;
            char[] invalid = Path.GetInvalidFileNameChars();
            StringBuilder builder = new StringBuilder(text.Length);
            foreach (char c in text)
            {
                UnicodeCategory category = char.GetUnicodeCategory(c);
                bool unusable = Array.IndexOf(invalid, c) >= 0 || category == UnicodeCategory.Control ||
                    category == UnicodeCategory.Format || c == ReplacementChar;
                builder.Append(unusable ? '_' : c);
            }
            string clean = builder.ToString().Trim(' ', '.');
            if (clean.Trim('_', ' ', '.').Length == 0) return string.Empty;
            if (clean.Length > maxChars)
            {
                int cut = maxChars;
                if (char.IsHighSurrogate(clean[cut - 1])) cut--;
                clean = clean.Substring(0, cut).TrimEnd(' ', '.');
            }
            return clean;
        }

        // 压缩包条目名是否无法可靠解码：含替换字符 U+FFFD，或含私用区字符（U+E000..U+F8FF）。
        // 正常的 GBK/UTF-8 文件名不会出现这些字符；GBK 解码器会把未定义的字节映射到私用区，所以一并视为不可靠。
        // 不可靠的名字不用于命名，退回按时间命名。
        internal static bool IsUnreliableName(string name)
        {
            if (name == null) return false;
            foreach (char c in name)
                if (c == ReplacementChar || (c >= (char)0xE000 && c <= (char)0xF8FF)) return true;
            return false;
        }

        // CON、NUL、COM1 等设备名即使带扩展名也无法作为文件名使用，前面补一条下划线。
        internal static string AvoidReservedDeviceName(string name)
        {
            if (string.IsNullOrEmpty(name)) return name;
            int dot = name.IndexOf('.');
            string head = (dot >= 0 ? name.Substring(0, dot) : name).TrimEnd(' ');
            foreach (string reserved in ReservedDeviceNames)
                if (string.Equals(head, reserved, StringComparison.OrdinalIgnoreCase)) return "_" + name;
            return name;
        }

        // 主名：原文件名去掉已知后缀并清理；没有可用内容时用“导入-yyyyMMdd-HHmmss”。
        internal static string BuildBaseName(string originalName, DateTime now, int maxChars)
        {
            string name = SanitizeFileNamePart(StripKnownSuffixes(LastSegment(originalName)), maxChars);
            if (name.Length == 0)
                name = "导入-" + now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture);
            return AvoidReservedDeviceName(name);
        }

        // 主名可用的最大字符数：不超过 80，也要保证“目录\主名-9999后缀”不超过系统路径上限。
        internal static int MaxBaseNameChars(string directory, string suffix)
        {
            int used;
            try { used = Path.GetFullPath(directory).TrimEnd('\\', '/').Length; }
            catch (Exception) { used = directory == null ? 0 : directory.Length; }
            int room = PathLimit - used - 1 - suffix.Length - "-9999".Length;
            return Math.Max(8, Math.Min(MaximumBaseNameChars, room));
        }

        // 把临时文件改名放到最终位置：重名则依次追加“-2”“-3”……，永不覆盖已有文件
        // （File.Move 目标已存在时抛 IOException，竞争创建同名文件也不会被覆盖）。
        internal static string MoveToUniquePath(string tempPath, string directory, string baseName, string suffix)
        {
            for (int index = 1; index <= 9999; index++)
            {
                string name = index == 1 ? baseName + suffix : baseName + "-" + index.ToString(CultureInfo.InvariantCulture) + suffix;
                string target = Path.Combine(directory, name);
                if (File.Exists(target) || Directory.Exists(target)) continue;
                try
                {
                    File.Move(tempPath, target);
                    return target;
                }
                catch (IOException)
                {
                    if (File.Exists(target) || Directory.Exists(target)) continue;
                    throw;
                }
            }
            throw new IOException("找不到可用的文件名。");
        }

        // 回放文件夹里已有内容完全相同的回放时返回它的路径：先比大小，大小相同再比 SHA-256；被占用的文件不参与比较。
        internal static string FindIdenticalReplay(string directory, string candidatePath)
        {
            long length = new FileInfo(candidatePath).Length;
            byte[] candidateHash = null;
            foreach (string existing in ReplayArchive.EnumerateReplayFiles(directory))
            {
                try
                {
                    if (new FileInfo(existing).Length != length) continue;
                    if (candidateHash == null) candidateHash = ReplayLaunchManifest.HashFile(candidatePath);
                    if (ReplayLaunchManifest.EqualsHash(ReplayLaunchManifest.HashFile(existing), candidateHash)) return existing;
                }
                catch (IOException) { /* 正在被录制或播放的文件无法比较，跳过。 */ }
                catch (UnauthorizedAccessException) { }
            }
            return null;
        }

        // 导入崩溃或被强制结束时可能遗留 <32 位十六进制>.importing 临时文件；清除超过 minimumAge 的这类文件。
        internal static void CleanStaleTemporaryFiles(string directory, TimeSpan minimumAge, DateTime utcNow)
        {
            try
            {
                foreach (string file in Directory.EnumerateFiles(directory, "*" + TemporarySuffix))
                {
                    if (!IsTemporaryName(Path.GetFileName(file))) continue;
                    try
                    {
                        if (utcNow - File.GetLastWriteTimeUtc(file) >= minimumAge) File.Delete(file);
                    }
                    catch (Exception) { /* 清理是尽力而为。 */ }
                }
            }
            catch (Exception) { /* 目录无法枚举时不影响导入。 */ }
        }

        private static bool IsTemporaryName(string name)
        {
            if (name == null || name.Length != 32 + TemporarySuffix.Length ||
                !name.EndsWith(TemporarySuffix, StringComparison.OrdinalIgnoreCase)) return false;
            for (int i = 0; i < 32; i++)
            {
                char c = name[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
            }
            return true;
        }

        private static string NewTemporaryPath(string directory)
        {
            return Path.Combine(directory, Guid.NewGuid().ToString("N") + TemporarySuffix);
        }

        private static void TryDelete(string path)
        {
            try { if (File.Exists(path)) File.Delete(path); }
            catch (Exception) { /* 临时文件无法删除时留待下次导入清理。 */ }
        }

        // ----- 校验 -----

        // 对临时文件做完整校验；返回 null 表示可以导入，否则为给玩家看的原因（detail 为技术细节）。
        // 依次：容器头与清单（Open）、游戏版本、帧流版本、完整的数据校验（ValidatePayload）；不检查原始地图。
        internal static string ValidateCandidate(string path, byte[] localGameExeHash, out ReplayArchive archive, out string detail)
        {
            archive = null;
            detail = null;
            try
            {
                ReplayArchive opened = ReplayArchive.Open(path);
                if (localGameExeHash == null) return GameUnreadableText;
                if (!ReplayLaunchManifest.EqualsHash(opened.GameExeHash, localGameExeHash)) return VersionMismatchText;
                if (!opened.HasPlayerInputEffects) return LegacyFormatText;
                opened.ValidatePayload();
                archive = opened;
                return null;
            }
            catch (InvalidDataException ex) { detail = ex.ToString(); return CorruptedReplayText; }
            catch (EndOfStreamException ex) { detail = ex.ToString(); return CorruptedReplayText; }
            catch (Exception ex) { detail = ex.ToString(); return DescribeFailure(ex); }
        }

        // 把文件系统异常翻译成玩家能看懂的话。
        internal static string DescribeFailure(Exception ex)
        {
            if (ex is FileNotFoundException || ex is DirectoryNotFoundException) return "找不到这个文件，可能已被移动或删除";
            if (ex is UnauthorizedAccessException) return "没有权限读取或保存文件，请换一个位置再试";
            if (ex is PathTooLongException) return "文件路径太长，无法保存";
            IOException io = ex as IOException;
            if (io != null)
            {
                int code = io.HResult & 0xFFFF; // ERROR_HANDLE_DISK_FULL=39，ERROR_DISK_FULL=112
                if (code == 112 || code == 39) return "磁盘空间不足，无法保存";
                return "读取或保存文件时出错，文件可能正被其他程序占用，或磁盘出了问题";
            }
            return "导入时出现意外错误";
        }

        // ----- 导入流程 -----

        // 依次导入 sources 中的文件（回放或 zip）；永不抛出异常，每一项的结果与原因都记在返回值里。
        public static ReplayImportResult Import(IEnumerable<string> sources, ReplayImportOptions options)
        {
            ReplayImportResult result = new ReplayImportResult();
            List<string> list = new List<string>();
            if (sources != null)
                foreach (string source in sources)
                    if (!string.IsNullOrEmpty(source)) list.Add(source);
            if (list.Count == 0) return result;
            string directory = options.ReplaysDirectory;
            try { Directory.CreateDirectory(directory); }
            catch (Exception ex)
            {
                foreach (string source in list) result.Skip(LastSegment(source), FolderUnavailableText, ex.ToString());
                return result;
            }
            CleanStaleTemporaryFiles(directory, TimeSpan.FromDays(1), DateTime.UtcNow);
            foreach (string source in list)
            {
                try { ImportSource(source, options, result); }
                catch (Exception ex) { result.Skip(LastSegment(source), DescribeFailure(ex), ex.ToString()); }
            }
            return result;
        }

        private static void ImportSource(string path, ReplayImportOptions options, ReplayImportResult result)
        {
            string name = LastSegment(path);
            ReplayContentKind kind;
            try { kind = ClassifyFile(path); }
            catch (Exception ex)
            {
                result.Skip(name, DescribeFailure(ex), ex.ToString());
                return;
            }
            switch (kind)
            {
                case ReplayContentKind.Replay:
                    ImportLooseReplay(path, name, options, result);
                    break;
                case ReplayContentKind.Zip:
                    ImportZip(path, name, options, result);
                    break;
                case ReplayContentKind.Rar:
                case ReplayContentKind.SevenZip:
                    result.Skip(name, UnsupportedArchiveText, null);
                    break;
                default:
                    result.Skip(name, NotReplayText, null);
                    break;
            }
        }

        private static void ImportLooseReplay(string path, string name, ReplayImportOptions options, ReplayImportResult result)
        {
            ReplayImportLimits limits = options.EffectiveLimits;
            string directory = options.ReplaysDirectory;
            string temp = NewTemporaryPath(directory);
            try
            {
                try
                {
                    using (FileStream source = OpenSource(path))
                    {
                        if (source.Length > limits.MaxEntryBytes)
                        {
                            result.Skip(name, TooLargeText, null);
                            return;
                        }
                        using (FileStream output = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None, BufferBytes))
                        {
                            long total = 0;
                            CopyBounded(source, output, limits.MaxEntryBytes, ref total, long.MaxValue);
                            output.Flush(true);
                        }
                    }
                }
                catch (LimitException)
                {
                    result.Skip(name, TooLargeText, null);
                    return;
                }
                catch (SourceReadException ex)
                {
                    result.Skip(name, UnreadableSourceText, ex.ToString());
                    return;
                }
                ReplayArchive archive;
                string detail;
                string reason = ValidateCandidate(temp, options.LocalGameExeHash, out archive, out detail);
                if (reason != null)
                {
                    result.Skip(name, reason, detail);
                    return;
                }
                LandReplay(new PendingReplay { TempPath = temp, Archive = archive, DisplayName = name, OriginalName = name },
                    options, result);
            }
            finally { TryDelete(temp); }
        }

        private static void ImportZip(string path, string zipName, ReplayImportOptions options, ReplayImportResult result)
        {
            if (options.OpenZip == null)
            {
                result.Skip(zipName, UnsupportedArchiveText, null);
                return;
            }
            IReplayZipArchive archive;
            try { archive = options.OpenZip(path); }
            catch (Exception ex)
            {
                bool ioProblem = (ex is IOException && !(ex is EndOfStreamException)) || ex is UnauthorizedAccessException;
                result.Skip(zipName, ioProblem ? DescribeFailure(ex) : CorruptedZipText, ex.ToString());
                return;
            }
            ReplayImportLimits limits = options.EffectiveLimits;
            string directory = options.ReplaysDirectory;
            List<PendingReplay> pending = new List<PendingReplay>();
            int firstSkip = result.Skipped.Count;
            try
            {
                using (archive)
                {
                    List<IReplayZipEntry> files = new List<IReplayZipEntry>();
                    foreach (IReplayZipEntry entry in archive.Entries)
                        if (!entry.IsDirectory) files.Add(entry);
                    if (files.Count > limits.MaxScannedEntries)
                        throw new LimitException("压缩包条目数 " + files.Count + " 超过上限 " + limits.MaxScannedEntries + "。");
                    foreach (IReplayZipEntry entry in files)
                    {
                        if (entry.IsEncrypted)
                        {
                            // 加密条目无法识别内容，整个压缩包按“有密码”处理，避免悄悄漏掉其中的回放。
                            result.Skip(zipName, EncryptedZipText, null);
                            return;
                        }
                    }

                    long total = 0;
                    int replayCount = 0;
                    int unreadable = 0;
                    foreach (IReplayZipEntry entry in files)
                    {
                        // 识别只读条目开头；无法读取的条目不能算作回放，也不作为错误逐个提示。
                        byte[] head = new byte[HeadBytes];
                        int headLength;
                        try
                        {
                            using (Stream stream = entry.Open()) headLength = ReadHead(stream, head);
                        }
                        catch (Exception)
                        {
                            unreadable++;
                            continue;
                        }
                        if (Classify(head, headLength) != ReplayContentKind.Replay) continue;

                        replayCount++;
                        if (replayCount > limits.MaxReplayEntries)
                            throw new LimitException("压缩包内的回放条目数超过上限 " + limits.MaxReplayEntries + "。");
                        if (entry.DeclaredBytes > limits.MaxEntryBytes)
                            throw new LimitException("压缩包条目声明的大小 " + entry.DeclaredBytes + " 超过上限 " + limits.MaxEntryBytes + "。");

                        string entryName = LastSegment(entry.Name);
                        bool nameUnreliable = IsUnreliableName(entryName);
                        string display = zipName + " 中的 " + (nameUnreliable || entryName.Length == 0 ? "（名称无法识别的回放）" : entryName);
                        string temp = NewTemporaryPath(directory);
                        bool kept = false;
                        try
                        {
                            try
                            {
                                using (Stream stream = entry.Open())
                                using (FileStream output = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None, BufferBytes))
                                {
                                    CopyBounded(stream, output, limits.MaxEntryBytes, ref total, limits.MaxTotalBytes);
                                    output.Flush(true);
                                }
                            }
                            catch (SourceReadException ex)
                            {
                                result.Skip(display, CorruptedEntryText, ex.ToString());
                                continue;
                            }
                            ReplayArchive replay;
                            string detail;
                            string reason = ValidateCandidate(temp, options.LocalGameExeHash, out replay, out detail);
                            if (reason != null)
                            {
                                result.Skip(display, reason, detail);
                                continue;
                            }
                            pending.Add(new PendingReplay
                            {
                                TempPath = temp,
                                Archive = replay,
                                DisplayName = display,
                                OriginalName = nameUnreliable ? string.Empty : entryName
                            });
                            kept = true;
                        }
                        finally
                        {
                            if (!kept) TryDelete(temp);
                        }
                    }

                    if (replayCount == 0)
                    {
                        result.Skip(zipName, unreadable > 0 ?
                            "压缩包里没有找到可以读取的回放（有 " + unreadable.ToString(CultureInfo.InvariantCulture) +
                            " 个文件读取失败），请让对方重新压缩，或直接发送回放文件" : NoReplayInZipText, null);
                        return;
                    }
                }

                // 整个压缩包都在限额内读完后才落地，超限的压缩包不会留下任何内容。
                foreach (PendingReplay replay in pending)
                {
                    try { LandReplay(replay, options, result); }
                    catch (Exception ex) { result.Skip(replay.DisplayName, DescribeFailure(ex), ex.ToString()); }
                }
            }
            catch (LimitException ex)
            {
                // 整个压缩包中止：只给出一条说明，不再逐条列出中途已记下的条目。
                result.RemoveSkippedFrom(firstSkip);
                result.Skip(zipName, ZipLimitText, ex.Message);
            }
            finally
            {
                foreach (PendingReplay replay in pending) TryDelete(replay.TempPath);
            }
        }

        // 把已通过校验的临时文件放进回放文件夹：内容相同的已有回放则跳过；否则按 flags 决定后缀并取不重名的最终名。
        private static void LandReplay(PendingReplay replay, ReplayImportOptions options, ReplayImportResult result)
        {
            string directory = options.ReplaysDirectory;
            string identical = FindIdenticalReplay(directory, replay.TempPath);
            if (identical != null)
            {
                result.Skip(replay.DisplayName, "已经导入过了（与“" + LastSegment(identical) + "”内容相同）", null);
                return;
            }
            string suffix = replay.Archive.IsUnfinalized ? ".yrrep.partial" : ".yrrep";
            string baseName = BuildBaseName(replay.OriginalName, options.CurrentTime, MaxBaseNameChars(directory, suffix));
            string target = MoveToUniquePath(replay.TempPath, directory, baseName, suffix);
            result.AddImported(Path.GetFullPath(target));
        }

        // 边读边计数，不信任任何头部声明的大小：单项超过 entryLimit 或累计超过 totalLimit 即抛 LimitException。
        // 读取源数据出错包装为 SourceReadException；写入目标出错的异常原样抛出。
        private static void CopyBounded(Stream input, Stream output, long entryLimit, ref long total, long totalLimit)
        {
            byte[] buffer = new byte[BufferBytes];
            long copied = 0;
            while (true)
            {
                int read;
                try { read = input.Read(buffer, 0, buffer.Length); }
                catch (Exception ex) { throw new SourceReadException(ex); }
                if (read <= 0) return;
                copied += read;
                total += read;
                if (copied > entryLimit) throw new LimitException("单个回放超过上限 " + entryLimit + " 字节。");
                if (total > totalLimit) throw new LimitException("回放合计超过上限 " + totalLimit + " 字节。");
                output.Write(buffer, 0, read);
            }
        }
    }
}
