using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using RevitVersion.Core;

namespace RevitVersion.Cli
{
    internal static class Program
    {
        private static int Main(string[] args)
        {
            Console.OutputEncoding = System.Text.Encoding.UTF8;

            if (args.Length == 0)
            {
                PrintUsage();
                return 1;
            }

            string first = args[0];

            if (first == "--diag")
            {
                Diag.Run(args[1]);
                return 0;
            }

            if (first == "--single")
            {
                if (args.Length < 2) { PrintUsage(); return 1; }
                return DumpSingle(args[1]);
            }

            if (first == "--json")
            {
                if (args.Length < 2) { PrintUsage(); return 1; }
                return DumpJson(args.Skip(1).ToArray());
            }

            if (first is "-h" or "--help" or "/?")
            {
                PrintUsage();
                return 0;
            }

            // 默认：对给定路径做递归扫描
            return ScanPaths(args);
        }

        private static void PrintUsage()
        {
            Console.WriteLine(@"rvtv - Revit 文件版本识别 CLI

用法:
  rvtv --single <file>          单文件详细信息
  rvtv --json <file|dir>...     JSON 输出
  rvtv <file|dir>...            递归扫描并列出表格

支持的扩展名: .rvt .rfa .rte .rft");
        }

        private static int DumpSingle(string path)
        {
            var info = RevitFileProbe.Probe(path);
            Console.WriteLine("FilePath       : " + info.FilePath);
            Console.WriteLine("Extension      : " + info.Extension);
            Console.WriteLine("FileSize       : " + info.FileSize.ToString("N0") + " bytes");
            Console.WriteLine("Success        : " + info.Success);
            Console.WriteLine("FormatVersion  : " + (info.FormatVersion?.ToString() ?? "-"));
            Console.WriteLine("RawFormat      : " + (info.RawFormatVersion ?? "-"));
            Console.WriteLine("ProductYear    : " + (info.ProductYear ?? "-"));
            Console.WriteLine("BuildVersion   : " + (info.BuildVersion ?? "-"));
            Console.WriteLine("Language       : " + (info.Language ?? "-"));
            Console.WriteLine("IsWorkshared   : " + info.IsWorkshared);
            Console.WriteLine("IsCentral      : " + info.IsCentral);
            Console.WriteLine("OriginalPath   : " + (info.OriginalPath ?? "-"));
            Console.WriteLine("Error          : " + (info.Error ?? "-"));
            return info.Success ? 0 : 2;
        }

        private static int DumpJson(string[] paths)
        {
            var files = ExpandPaths(paths);
            var results = files.Select(RevitFileProbe.Probe).ToList();
            Console.WriteLine(JsonWriter.Write(results));
            return 0;
        }

        private static int ScanPaths(string[] paths)
        {
            var files = ExpandPaths(paths);
            if (files.Count == 0)
            {
                Console.WriteLine("未找到 Revit 文件。");
                return 1;
            }

            int ok = 0, fail = 0;
            Console.WriteLine($"{"版本",-10} {"大小",12} {"构建号",-24} {"工作共享",-10} 文件名");
            Console.WriteLine(new string('-', 110));

            foreach (var f in files)
            {
                var info = RevitFileProbe.Probe(f);
                if (info.Success) ok++; else fail++;

                string ver = info.Success ? info.ShortLabel : "!";
                string size = FormatSize(info.FileSize);
                string build = info.BuildVersion ?? "-";
                string ws = info.IsWorkshared ? (info.IsCentral ? "Central" : "Local") : "-";
                Console.WriteLine($"{ver,-10} {size,12} {build,-24} {ws,-10} {Path.GetFileName(f)}");
            }

            Console.WriteLine(new string('-', 110));
            Console.WriteLine($"共 {files.Count} 个文件，成功 {ok}，失败 {fail}");
            return fail == 0 ? 0 : 2;
        }

        private static List<string> ExpandPaths(string[] paths)
        {
            var found = new List<string>();
            foreach (var p in paths)
            {
                if (File.Exists(p))
                {
                    found.Add(p);
                }
                else if (Directory.Exists(p))
                {
                    try
                    {
                        found.AddRange(Directory.EnumerateFiles(p, "*.*", SearchOption.AllDirectories)
                            .Where(RevitFileProbe.IsRevitExtension));
                    }
                    catch (Exception ex)
                    {
                        Console.Error.WriteLine($"[warn] 枚举目录失败 {p}: {ex.Message}");
                    }
                }
                else
                {
                    Console.Error.WriteLine($"[warn] 路径不存在: {p}");
                }
            }
            return found;
        }

        private static string FormatSize(long bytes)
        {
            string[] units = { "B", "KB", "MB", "GB" };
            double v = bytes;
            int u = 0;
            while (v >= 1024 && u < units.Length - 1) { v /= 1024; u++; }
            return v.ToString(u == 0 ? "0" : "0.0") + " " + units[u];
        }
    }
}
