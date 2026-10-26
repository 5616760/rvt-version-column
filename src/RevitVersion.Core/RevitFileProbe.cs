using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace RevitVersion.Core
{
    /// <summary>
    /// Revit 文件（.rvt / .rfa）版本识别结果。
    /// </summary>
    public sealed class RevitFileInfo
    {
        /// <summary>文件完整路径</summary>
        public string FilePath { get; internal set; }

        /// <summary>文件扩展名（小写，含点），如 ".rvt" / ".rfa"</summary>
        public string Extension { get; internal set; }

        /// <summary>文件大小（字节）</summary>
        public long FileSize { get; internal set; }

        /// <summary>是否存在 BasicFileInfo 流</summary>
        public bool HasBasicFileInfo { get; internal set; }

        /// <summary>格式版本号，例如 2021；无法读取为 null</summary>
        public int? FormatVersion { get; internal set; }

        /// <summary>原始格式版本字符串（BasicFileInfo 第一个字段）</summary>
        public string RawFormatVersion { get; internal set; }

        /// <summary>Revit 构建号，例如 "20200220_1100(x64)"</summary>
        public string BuildVersion { get; internal set; }

        /// <summary>保存/打开该文件时的本地完整路径（来自 BasicFileInfo）</summary>
        public string OriginalPath { get; internal set; }

        /// <summary>最后保存该文件的 Revit 构建信息（若可解析）</summary>
        public string SavedBy { get; internal set; }

        /// <summary>文件语言（例如 "CHS"）</summary>
        public string Language { get; internal set; }

        /// <summary>是否处于工作共享（worksharing）状态</summary>
        public bool IsWorkshared { get; internal set; }

        /// <summary>是否为中央模型</summary>
        public bool IsCentral { get; internal set; }

        /// <summary>产品年份标签，例如 "2021"；历史版本可能是 "15.0 / 2012"</summary>
        public string ProductYear => FormatVersion.HasValue
            ? RevitVersionMap.ResolveProductYear(FormatVersion.Value)
            : null;

        /// <summary>
        /// 纯年份数字，例如 "2012"。用于文件名、图标键、排序键等需要稳定短标识的场景。
        /// </summary>
        public string YearNumber => FormatVersion.HasValue
            ? RevitVersionMap.ResolveYearNumber(FormatVersion.Value)
            : null;

        /// <summary>解析失败原因；成功时 null</summary>
        public string Error { get; internal set; }

        /// <summary>是否成功识别出格式版本</summary>
        public bool Success => FormatVersion.HasValue && Error == null;

        /// <summary>
        /// 用于资源管理器列显示的短标签，例如 "2021"
        /// </summary>
        public string ShortLabel
        {
            get
            {
                if (ProductYear != null) return ProductYear;
                if (FormatVersion.HasValue) return "v" + FormatVersion.Value;
                return string.Empty;
            }
        }

        public override string ToString()
        {
            if (!Success)
                return string.IsNullOrEmpty(Error) ? "Unknown" : "Error: " + Error;

            var sb = new StringBuilder();
            sb.Append(ShortLabel);
            if (!string.IsNullOrEmpty(BuildVersion)) sb.Append(" (").Append(BuildVersion).Append(')');
            if (IsWorkshared) sb.Append(IsCentral ? " [Central]" : " [Workshared]");
            return sb.ToString();
        }
    }

    /// <summary>
    /// RVT / RFA 文件版本探测器。
    /// 只读取 CFB 目录 + BasicFileInfo 流，不加载模型数据。
    /// </summary>
    public static class RevitFileProbe
    {
        private const string BasicFileInfoStream = "BasicFileInfo";

        private static readonly HashSet<string> RevitExtensions =
            new HashSet<string>(StringComparer.OrdinalIgnoreCase) { ".rvt", ".rfa", ".rte", ".rft" };

        /// <summary>
        /// 是否为本工具处理的文件类型（按扩展名判断）。
        /// </summary>
        public static bool IsRevitExtension(string path)
        {
            if (string.IsNullOrEmpty(path)) return false;
            string ext = Path.GetExtension(path);
            return !string.IsNullOrEmpty(ext) && RevitExtensions.Contains(ext);
        }

        /// <summary>
        /// 读取指定文件的 Revit 版本信息。永不抛异常；失败信息写入 Error。
        /// </summary>
        public static RevitFileInfo Probe(string path)
        {
            var info = new RevitFileInfo
            {
                FilePath = path,
                Extension = Path.GetExtension(path)?.ToLowerInvariant(),
            };

            if (!File.Exists(path))
            {
                info.Error = "File not found";
                return info;
            }

            try
            {
                info.FileSize = new FileInfo(path).Length;

                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read,
                           FileShare.ReadWrite | FileShare.Delete, 4096, FileOptions.RandomAccess))
                using (var cfb = new CompoundFileReader(fs))
                {
                    if (!cfb.HasStream(BasicFileInfoStream))
                    {
                        info.Error = "No BasicFileInfo stream (legacy RVT pre-2010 or not a Revit file)";
                        return info;
                    }

                    info.HasBasicFileInfo = true;
                    byte[] raw = cfb.ReadStream(BasicFileInfoStream);
                    ParseBasicFileInfo(raw, info);
                }
            }
            catch (Exception ex)
            {
                info.Error = ex.GetType().Name + ": " + ex.Message;
            }

            return info;
        }

        /// <summary>
        /// 解析 BasicFileInfo 流内容。
        ///
        /// 实测布局（Revit 2021 文件，小端）——注意前缀是「字节数」而非字符数：
        ///   +0   u32 14                  → 版本字符串字节数
        ///   +4   byte[10] 保留全 0
        ///   +14  UTF-16LE "2021" (4 字节)
        ///   +18  u32 0x12 = 18           → 构建号字节数
        ///   +22  UTF-16LE "20200220_1100(x64)"
        ///   +..  u32 byteLen             → 保存路径字节数
        ///   +..  UTF-16LE 完整路径
        ///   …… 之后为 GUID / 语言 / 工作共享标记等，顺序随 Revit 版本略有差异
        ///
        /// 为兼容不同 Revit 版本，这里采用「首个 u32 即版本字节数、紧跟版本串」的
        /// 主路径，失败时回退到按字节扫描查找 4 位年份数字。
        /// </summary>
        private static void ParseBasicFileInfo(byte[] raw, RevitFileInfo info)
        {
            if (raw == null || raw.Length < 8)
            {
                info.Error = "BasicFileInfo stream too short";
                return;
            }

            if (TryParseStructured(raw, info)) return;

            // 回退：直接扫描 4 位年份的 UTF-16 文本
            if (TryScanFallback(raw, info)) return;

            info.Error = "Cannot read format version from BasicFileInfo";
        }

        /// <summary>
        /// 按已知结构化布局解析。
        ///
        /// 实测偏移（Revit 2021，BasicFileInfo 2187 字节）：
        ///   0..3    u32 14        ← 版本字段总长度（4 字节前缀 + 10 字节保留 + 版本串）
        ///   4..13   byte[10] 全 0  ← 保留区
        ///   14..17  u32 4         ← 版本串字节数
        ///   18..25  "2021" (UTF-16LE)
        ///   26..29  u32 0x12      ← 构建号字节数
        ///   30..    "20200220_1100(x64)"
        ///   后续    路径 / GUID / 语言 / 工作共享标记
        /// </summary>
        private static bool TryParseStructured(byte[] raw, RevitFileInfo info)
        {
            int fieldLen = BitConverter.ToInt32(raw, 0);
            if (fieldLen <= 0 || fieldLen > 512) return false;

            // 版本串：偏移 14 处为字符数，数据占 18..(18 + chars*2)
            if (14 + 4 > raw.Length) return false;
            int versionChars = BitConverter.ToInt32(raw, 14);
            if (versionChars <= 0 || versionChars > 32) return false;
            int versionByteLen = versionChars * 2;
            if (18 + versionByteLen > raw.Length) return false;

            string v = Encoding.Unicode.GetString(raw, 18, versionByteLen).Trim();
            if (!LooksLikeFormatVersion(v)) return false;

            info.RawFormatVersion = v;
            if (!int.TryParse(v, out int fv)) return false;
            info.FormatVersion = fv;

            // 构建号长度前缀位于 26（= 18 + 4 字符*2 + 4），实测固定
            int p = 26;
            if (p + 4 <= raw.Length)
            {
                int buildChars = BitConverter.ToInt32(raw, p);
                bool ok = buildChars > 0 && buildChars <= 256 && p + 4 + (long)buildChars * 2 <= raw.Length;

                if (ok)
                {
                    info.BuildVersion = ReadLengthPrefixedString(raw, ref p);
                    info.OriginalPath = ReadLengthPrefixedString(raw, ref p);
                }
                else
                {
                    info.BuildVersion = ScanBuildString(raw, 18 + versionByteLen);
                }
            }
            TryParseTrailingFields(raw, ref p, info);
            return true;
        }

        /// <summary>
        /// 回退路径：在流内扫描形如 "20xx" 的 UTF-16 版本号。
        /// </summary>
        private static bool TryScanFallback(byte[] raw, RevitFileInfo info)
        {
            for (int i = 0; i + 8 <= raw.Length; i += 2)
            {
                char c0 = (char)BitConverter.ToUInt16(raw, i);
                char c1 = (char)BitConverter.ToUInt16(raw, i + 2);
                char c2 = (char)BitConverter.ToUInt16(raw, i + 4);
                char c3 = (char)BitConverter.ToUInt16(raw, i + 6);
                if (c0 != '2' || c1 != '0' || !char.IsDigit(c2) || !char.IsDigit(c3)) continue;

                string v = new string(new[] { c0, c1, c2, c3 });
                if (int.TryParse(v, out int fv) && fv >= 2004 && fv <= 2100)
                {
                    info.RawFormatVersion = v;
                    info.FormatVersion = fv;
                    return true;
                }
            }
            return false;
        }

        /// <summary>
        /// 从指定位置起扫描形如 "YYYYMMDD_HHHH(x64)" 的构建号字符串。
        /// 用于 BasicFileInfo 内部偏移随 Revit 版本漂移时兜底。
        /// </summary>
        private static string ScanBuildString(byte[] raw, int start)
        {
            for (int i = Math.Max(0, start); i + 16 <= raw.Length; i += 2)
            {
                // 形如 8 位数字 + '_'
                bool digits = true;
                for (int k = 0; k < 8; k++)
                {
                    char c = (char)BitConverter.ToUInt16(raw, i + k * 2);
                    if (c < '0' || c > '9') { digits = false; break; }
                }
                if (!digits) continue;

                char sep = (char)BitConverter.ToUInt16(raw, i + 16);
                if (sep != '_') continue;

                // 向后取连续可打印 ASCII
                var sb = new StringBuilder();
                int q = i;
                while (q + 2 <= raw.Length && sb.Length < 64)
                {
                    char c = (char)BitConverter.ToUInt16(raw, q);
                    if (c < 0x20 || c > 0x7E) break;
                    sb.Append(c);
                    q += 2;
                }
                if (sb.Length >= 10) return sb.ToString();
            }
            return null;
        }

        private static bool LooksLikeFormatVersion(string s)
        {
            if (string.IsNullOrEmpty(s) || s.Length != 4) return false;
            if (!int.TryParse(s, out int v)) return false;
            return v >= 2000 && v <= 2100;
        }

        private static void TryParseTrailingFields(byte[] raw, ref int pos, RevitFileInfo info)
        {
            try
            {
                // 跳过一个 GUID 字段（长度前缀 + 内容），通常为全 0
                ReadLengthPrefixedString(raw, ref pos);
                ReadLengthPrefixedString(raw, ref pos);

                // 语言标记，例如 "CHS"
                string lang = ReadLengthPrefixedString(raw, ref pos);
                if (!string.IsNullOrEmpty(lang) && lang.Length <= 8)
                    info.Language = lang;

                // 其余内容用于判定工作共享状态
                int start = Math.Min(Math.Max(pos, 0), raw.Length);
                int tailLen = raw.Length - start;
                if (tailLen < 2) return;
                if ((tailLen & 1) == 1) tailLen--;
                string tail = Encoding.Unicode.GetString(raw, start, tailLen);

                if (tail.IndexOf("Worksharing", StringComparison.OrdinalIgnoreCase) >= 0)
                    info.IsWorkshared = true;
                if (tail.IndexOf("Central", StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    info.IsWorkshared = true;
                    info.IsCentral = true;
                }
            }
            catch
            {
                // 尾部字段仅为增强信息，失败不报错
            }
        }

        private static string ReadLengthPrefixedString(byte[] raw, ref int pos)
        {
            if (pos + 4 > raw.Length) return null;

            // 前缀为「字符数」（UTF-16 每字符 2 字节）。实测：
            //   构建号 18 字符 → 前缀 0x12，实际占 36 字节。
            int chars = BitConverter.ToInt32(raw, pos);
            if (chars <= 0 || chars > 4096) return null;
            long byteLen = (long)chars * 2;
            if (pos + 4 + byteLen > raw.Length) return null;

            string s = Encoding.Unicode.GetString(raw, pos + 4, (int)byteLen);
            pos += 4 + (int)byteLen;
            return s;
        }
    }
}
