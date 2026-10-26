using System;
using System.IO;
using System.Reflection;
using System.Text;

namespace RevitVersion.Cli
{
    internal static class Diag
    {
        public static void Run(string path)
        {
            var asm = typeof(RevitVersion.Core.RevitFileProbe).Assembly;
            var t = asm.GetType("RevitVersion.Core.CompoundFileReader");
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            var reader = Activator.CreateInstance(t, new object[] { fs });
            var readStream = t.GetMethod("ReadStream", BindingFlags.Public | BindingFlags.Instance);
            var bfi = (byte[])readStream.Invoke(reader, new object[] { "BasicFileInfo" });
            ((IDisposable)reader).Dispose();
            // 26: buildLen=18 chars -> bytes = 36
            int p = 26;
            int chars = BitConverter.ToInt32(bfi, p);
            string build = Encoding.Unicode.GetString(bfi, p + 4, chars * 2);
            Console.WriteLine($"build(chars={chars}) = " + build);
            int q = p + 4 + chars * 2;
            int pchars = BitConverter.ToInt32(bfi, q);
            if (pchars > 0 && pchars < 4096)
                Console.WriteLine($"path(chars={pchars}) = " + Encoding.Unicode.GetString(bfi, q + 4, pchars * 2));
            Console.WriteLine("q=" + q + " pchars=" + pchars);
        }
    }
}
