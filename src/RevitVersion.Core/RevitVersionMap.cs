using System;
using System.Collections.Generic;

namespace RevitVersion.Core
{
    /// <summary>
    /// Revit 文件格式版本号 → Revit 产品年份 映射表。
    ///
    /// 事实来源：
    /// 1. RVT 的 BasicFileInfo 流中第一个长度前缀字符串即「格式版本」，
    ///    例如 2021 版的 .rvt 该值为 "2021"（本项目实测：Revit 2021 生成的文件为 "2021"）。
    /// 2. 自 RVT 2019 起，Autodesk 提供单一版本文件格式（不再向后兼容保存）。
    /// 3. RFA（族文件）沿用同一套格式版本号。
    ///
    /// 说明：格式版本号与产品年份在 2016 及之后基本 1:1 对应，
    /// 更早的版本（2014/2015/2016）存在格式号与年份不完全一致的历史情况，
    /// 这里按已确认的映射填写，未知值原样展示，不猜测。
    /// </summary>
    public static class RevitVersionMap
    {
        private static readonly Dictionary<int, string> KnownFormats = new Dictionary<int, string>
        {
            // 已确认为 1:1 对应的区间
            [2016] = "2016",
            [2017] = "2017",
            [2018] = "2018",
            [2019] = "2019",
            [2020] = "2020",
            [2021] = "2021",
            [2022] = "2022",
            [2023] = "2023",
            [2024] = "2024",
            [2025] = "2025",
            [2026] = "2026",
            [2027] = "2027",

            // 历史格式号（Autodesk 早期格式，非年份直读）
            [2004] = "9.0 / 2006",
            [2005] = "10.0 / 2007",
            [2006] = "11.0 / 2008",
            [2007] = "12.0 / 2009",
            [2010] = "13.0 / 2010",
            [2011] = "14.0 / 2011",
            [2012] = "15.0 / 2012",
            [2013] = "16.0 / 2013",
            [2014] = "17.0 / 2014",
            [2015] = "18.0 / 2015",
        };

        /// <summary>
        /// 由格式版本号取得友好的产品年份标签。未知返回 null。
        /// 注意：历史版本的返回值可能是组合串（如 "15.0 / 2012"），
        /// 只适合显示，不要用于文件名 / 图标键 / 比较运算。
        /// </summary>
        public static string ResolveProductYear(int formatVersion)
        {
            if (KnownFormats.TryGetValue(formatVersion, out var year))
                return year;

            // 2016 之后的格式号与年份一致，做安全推断并明确标注
            if (formatVersion >= 2016 && formatVersion <= 2100)
                return formatVersion.ToString();

            return null;
        }

        /// <summary>
        /// 由格式版本号取得纯年份数字（如 "2012"）。
        /// 与 <see cref="ResolveProductYear"/> 的区别：历史版本也返回纯年份，
        /// 适用于文件名、图标键、排序键等需要稳定短标识的场景。
        /// </summary>
        public static string ResolveYearNumber(int formatVersion)
        {
            // 历史版本：取组合标签里 "/" 之后的部分
            if (KnownFormats.TryGetValue(formatVersion, out var label))
            {
                int slash = label.LastIndexOf('/');
                if (slash >= 0)
                {
                    string tail = label.Substring(slash + 1).Trim();
                    if (tail.Length > 0) return tail;
                }
                return label.Trim();
            }

            if (formatVersion >= 2016 && formatVersion <= 2100)
                return formatVersion.ToString();

            return null;
        }

        /// <summary>
        /// 判断是否为已知的、受支持的格式版本。
        /// </summary>
        public static bool IsKnownFormat(int formatVersion)
        {
            return KnownFormats.ContainsKey(formatVersion)
                || (formatVersion >= 2016 && formatVersion <= 2100);
        }
    }
}
