using System.Collections.Generic;
using System.Text;
using RevitVersion.Core;

namespace RevitVersion.Cli
{
    internal static class JsonWriter
    {
        public static string Write(IEnumerable<RevitFileInfo> items)
        {
            var sb = new StringBuilder();
            sb.Append('[');
            bool first = true;
            foreach (var i in items)
            {
                if (!first) sb.Append(',');
                first = false;
                sb.Append('{');
                AppendPair(sb, "filePath", i.FilePath, true);
                AppendPair(sb, "extension", i.Extension, false);
                sb.Append(",\"fileSize\":").Append(i.FileSize);
                sb.Append(",\"success\":").Append(i.Success ? "true" : "false");
                sb.Append(",\"formatVersion\":").Append(i.FormatVersion?.ToString() ?? "null");
                AppendPair(sb, "rawFormatVersion", i.RawFormatVersion, false);
                AppendPair(sb, "productYear", i.ProductYear, false);
                AppendPair(sb, "buildVersion", i.BuildVersion, false);
                AppendPair(sb, "language", i.Language, false);
                sb.Append(",\"isWorkshared\":").Append(i.IsWorkshared ? "true" : "false");
                sb.Append(",\"isCentral\":").Append(i.IsCentral ? "true" : "false");
                AppendPair(sb, "originalPath", i.OriginalPath, false);
                AppendPair(sb, "error", i.Error, false);
                sb.Append('}');
            }
            sb.Append(']');
            return sb.ToString();
        }

        private static void AppendPair(StringBuilder sb, string key, string value, bool isFirst)
        {
            if (!isFirst) sb.Append(',');
            sb.Append('"').Append(key).Append("\":");
            if (value == null) sb.Append("null");
            else sb.Append('"').Append(Escape(value)).Append('"');
        }

        private static string Escape(string s)
        {
            var sb = new StringBuilder(s.Length + 8);
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                        else sb.Append(c);
                        break;
                }
            }
            return sb.ToString();
        }
    }
}
