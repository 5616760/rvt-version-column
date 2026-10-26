using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace RevitVersion.Core
{
    /// <summary>
    /// 最小化的 OLE2 / CFB (Compound File Binary) 只读读取器。
    /// 仅支持读取：目录项、常规扇区链、迷你扇区链、迷你流。
    /// 不写回、不解析属性集以外内容。用于读取 .rvt / .rfa 内的 BasicFileInfo 流。
    ///
    /// 参考：[MS-CFB] Compound File Binary File Format。
    /// </summary>
    internal sealed class CompoundFileReader : IDisposable
    {
        private const uint MaxRegSect = 0xFFFFFFFA;
        private const uint EndOfChain = 0xFFFFFFFE;
        private const uint FreeSect   = 0xFFFFFFFF;
        private const uint NoStream   = 0xFFFFFFFF;

        private readonly Stream _stream;
        private readonly int _sectorSize;
        private readonly int _miniSectorSize;
        private readonly uint _miniStreamCutoff;

        private readonly uint[] _fat;        // regular FAT (sector index -> next sector)
        private readonly uint[] _miniFat;
        private readonly List<DirectoryEntry> _entries = new List<DirectoryEntry>();

        private byte[] _miniStream;

        internal sealed class DirectoryEntry
        {
            public string Name;
            public byte Type;      // 0=empty 1=storage 2=stream 5=root
            public uint StartSector;
            public long Size;
        }

        public CompoundFileReader(Stream stream)
        {
            _stream = stream ?? throw new ArgumentNullException(nameof(stream));
            if (!_stream.CanSeek) throw new ArgumentException("Stream must be seekable.", nameof(stream));

            byte[] header = new byte[512];
            ReadAt(0, header, 512);

            // Signature D0 CF 11 E0 A1 B1 1A E1
            if (header[0] != 0xD0 || header[1] != 0xCF || header[2] != 0x11 || header[3] != 0xE0 ||
                header[4] != 0xA1 || header[5] != 0xB1 || header[6] != 0x1A || header[7] != 0xE1)
            {
                throw new InvalidDataException("Not a Compound File Binary (OLE2) container.");
            }

            ushort sectorShift = BitConverter.ToUInt16(header, 30);
            ushort miniSectorShift = BitConverter.ToUInt16(header, 32);
            if (sectorShift != 9 && sectorShift != 12)
                throw new InvalidDataException("Unsupported sector shift: " + sectorShift);

            _sectorSize = 1 << sectorShift;
            _miniSectorSize = 1 << miniSectorShift;
            _miniStreamCutoff = BitConverter.ToUInt32(header, 56);

            int fatSectorCount = BitConverter.ToInt32(header, 44);
            uint firstDirSector = BitConverter.ToUInt32(header, 48);
            uint firstMiniFatSector = BitConverter.ToUInt32(header, 60);
            int miniFatSectorCount = BitConverter.ToInt32(header, 64);
            uint firstDifatSector = BitConverter.ToUInt32(header, 68);
            int difatSectorCount = BitConverter.ToInt32(header, 72);

            // ---- DIFAT: 109 entries in header + chained DIFAT sectors ----
            var fatSectorIds = new List<uint>(fatSectorCount);
            for (int i = 0; i < 109 && fatSectorIds.Count < fatSectorCount; i++)
            {
                uint sid = BitConverter.ToUInt32(header, 76 + i * 4);
                if (sid > MaxRegSect) break;
                fatSectorIds.Add(sid);
            }

            uint difatSector = firstDifatSector;
            int guard = 0;
            while (difatSector <= MaxRegSect && difatSectorCount > 0 && guard++ < difatSectorCount + 8)
            {
                byte[] buf = ReadSector(difatSector);
                int perSector = _sectorSize / 4 - 1;
                for (int i = 0; i < perSector && fatSectorIds.Count < fatSectorCount; i++)
                {
                    uint sid = BitConverter.ToUInt32(buf, i * 4);
                    if (sid > MaxRegSect) break;
                    fatSectorIds.Add(sid);
                }
                difatSector = BitConverter.ToUInt32(buf, _sectorSize - 4);
            }

            // ---- FAT ----
            int entriesPerSector = _sectorSize / 4;
            var fat = new List<uint>(fatSectorIds.Count * entriesPerSector);
            foreach (uint fsid in fatSectorIds)
            {
                byte[] buf = ReadSector(fsid);
                for (int i = 0; i < entriesPerSector; i++)
                    fat.Add(BitConverter.ToUInt32(buf, i * 4));
            }
            _fat = fat.ToArray();

            // ---- Directory ----
            // 目录长度由链本身决定，不从头部 Size 字段推断（部分写入器该字段为 0）
            byte[] dirData = ReadChain(firstDirSector, 0);
            for (int off = 0; off + 128 <= dirData.Length; off += 128)
            {
                var e = ParseDirectoryEntry(dirData, off);
                if (e != null) _entries.Add(e);
            }

            // ---- MiniFAT ----
            if (firstMiniFatSector <= MaxRegSect)
            {
                byte[] miniFatData = ReadChain(firstMiniFatSector, 0);
                var mf = new uint[miniFatData.Length / 4];
                for (int i = 0; i < mf.Length; i++) mf[i] = BitConverter.ToUInt32(miniFatData, i * 4);
                _miniFat = mf;
            }
            else
            {
                _miniFat = new uint[0];
            }

            // ---- Root entry / mini stream ----
            if (_entries.Count > 0 && _entries[0].Type == 5 && _entries[0].Size > 0)
            {
                _miniStream = ReadChain(_entries[0].StartSector, _entries[0].Size);
            }
        }

        private DirectoryEntry ParseDirectoryEntry(byte[] dir, int off)
        {
            int nameLen = BitConverter.ToUInt16(dir, off + 64);
            byte type = dir[off + 66];
            if (type == 0) return null;
            if (nameLen < 2 || nameLen > 64) return null;

            string name = Encoding.Unicode.GetString(dir, off, nameLen - 2);

            uint start = BitConverter.ToUInt32(dir, off + 116);
            // Size is 8 bytes (v3 uses low 4 bytes)
            long size = BitConverter.ToUInt32(dir, off + 120);

            return new DirectoryEntry { Name = name, Type = type, StartSector = start, Size = size };
        }

        public bool HasStream(string name)
        {
            return FindEntry(name) != null;
        }

        private DirectoryEntry FindEntry(string name)
        {
            foreach (var e in _entries)
            {
                if (e.Type == 2 && string.Equals(e.Name, name, StringComparison.OrdinalIgnoreCase))
                    return e;
            }
            return null;
        }

        public byte[] ReadStream(string name)
        {
            var e = FindEntry(name);
            if (e == null) return null;

            if (e.Size < _miniStreamCutoff)
            {
                if (_miniStream == null) return new byte[0];
                return ReadMiniChain(e.StartSector, e.Size);
            }
            return ReadChain(e.StartSector, e.Size);
        }

        private byte[] ReadMiniChain(uint startSector, long size)
        {
            if (size <= 0) return new byte[0];
            var output = new byte[size];
            long written = 0;
            uint sector = startSector;
            int guard = 0;
            int maxChain = _miniFat.Length + 1;

            while (sector <= MaxRegSect && written < size && guard++ < maxChain)
            {
                long offset = (long)sector * _miniSectorSize;
                if (offset + _miniSectorSize > _miniStream.Length) break;
                int toCopy = (int)Math.Min(_miniSectorSize, size - written);
                Buffer.BlockCopy(_miniStream, (int)offset, output, (int)written, toCopy);
                written += toCopy;

                if (sector >= _miniFat.Length) break;
                sector = _miniFat[sector];
            }
            if (written < size) Array.Resize(ref output, (int)written);
            return output;
        }

        /// <summary>
        /// 沿 FAT 链读取扇区。size &gt; 0 时截断到指定长度；
        /// size &lt;= 0 表示「读到链结束」（用于 MiniFAT 等长度未知的结构）。
        /// </summary>
        private byte[] ReadChain(uint startSector, long size)
        {
            bool bounded = size > 0;
            var output = new List<byte>(bounded && size < 64L * 1024 * 1024 ? (int)size : 4096);
            uint sector = startSector;
            int guard = 0;
            int maxSectors = _fat.Length + 1;

            while (sector <= MaxRegSect && guard++ < maxSectors)
            {
                if (bounded && output.Count >= size) break;

                byte[] buf = ReadSector(sector);
                if (bounded)
                {
                    int remaining = (int)(size - output.Count);
                    if (remaining <= 0) break;
                    if (remaining < buf.Length) output.AddRange(new ArraySegment<byte>(buf, 0, remaining));
                    else output.AddRange(buf);
                }
                else
                {
                    output.AddRange(buf);
                }

                if (sector >= _fat.Length) break;
                sector = _fat[sector];
            }

            if (bounded && output.Count > size) output.RemoveRange((int)size, output.Count - (int)size);
            return output.ToArray();
        }

        private byte[] ReadSector(uint sectorId)
        {
            // 扇区起始偏移恒为 sectorSize 的整数倍。
            // v3(sectorShift=9) 时首个扇区在 512；v4(sectorShift=12) 时头部被补齐到 4096，
            // 首个扇区在 4096。二者都由 "偏移 = sectorSize * (sectorId + 1)" 统一表达。
            byte[] buf = new byte[_sectorSize];
            long offset = (long)_sectorSize * ((long)sectorId + 1);
            ReadAt(offset, buf, _sectorSize);
            return buf;
        }

        private void ReadAt(long offset, byte[] buffer, int count)
        {
            _stream.Seek(offset, SeekOrigin.Begin);
            int read = 0;
            while (read < count)
            {
                int n = _stream.Read(buffer, read, count - read);
                if (n <= 0) break;
                read += n;
            }
        }

        public void Dispose()
        {
            _stream?.Dispose();
        }
    }
}
