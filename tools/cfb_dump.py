"""BasicFileInfo 结构探查工具（诊断用，不参与发布）。

用法：
    python tools/cfb_dump.py "D:\\path\\model.rvt"
"""
import struct
import sys


def dump(path):
    f = open(path, 'rb')
    hdr = f.read(512)
    ss = 1 << struct.unpack_from('<H', hdr, 30)[0]
    mss = 1 << struct.unpack_from('<H', hdr, 32)[0]
    cutoff = struct.unpack_from('<I', hdr, 56)[0]
    fat_cnt = struct.unpack_from('<i', hdr, 44)[0]
    dir_start = struct.unpack_from('<I', hdr, 48)[0]
    mf_start = struct.unpack_from('<I', hdr, 60)[0]
    difat_start = struct.unpack_from('<I', hdr, 68)[0]
    difat_cnt = struct.unpack_from('<i', hdr, 72)[0]

    print("sector=%d mini=%d cutoff=%d fat_cnt=%d dir=%d" % (ss, mss, cutoff, fat_cnt, dir_start))

    def read_sector(sid):
        f.seek(ss * (sid + 1))
        return f.read(ss)

    fat_ids = []
    for i in range(109):
        sid = struct.unpack_from('<I', hdr, 76 + i * 4)[0]
        if sid > 0xFFFFFFFA:
            break
        fat_ids.append(sid)

    cur, guard = difat_start, 0
    while cur <= 0xFFFFFFFA and difat_cnt > 0 and guard < difat_cnt + 8 and len(fat_ids) < fat_cnt:
        buf = read_sector(cur)
        for i in range(ss // 4 - 1):
            sid = struct.unpack_from('<I', buf, i * 4)[0]
            if sid > 0xFFFFFFFA:
                break
            fat_ids.append(sid)
        cur = struct.unpack_from('<I', buf, ss - 4)[0]
        guard += 1

    fat = []
    for sid in fat_ids:
        buf = read_sector(sid)
        for i in range(ss // 4):
            fat.append(struct.unpack_from('<I', buf, i * 4)[0])

    def read_chain(start, size):
        out = bytearray()
        sid, g = start, 0
        while sid <= 0xFFFFFFFA and g < len(fat) + 1:
            if size > 0 and len(out) >= size:
                break
            buf = read_sector(sid)
            out += buf[:size - len(out)] if size > 0 else buf
            if sid >= len(fat):
                break
            sid = fat[sid]
            g += 1
        return bytes(out)

    dirdata = read_chain(dir_start, 0)
    entries = []
    for off in range(0, len(dirdata) - 127, 128):
        nlen = struct.unpack_from('<H', dirdata, off + 64)[0]
        typ = dirdata[off + 66]
        if typ == 0 or nlen < 2 or nlen > 64:
            continue
        name = dirdata[off:off + nlen - 2].decode('utf-16-le', 'replace')
        start = struct.unpack_from('<I', dirdata, off + 116)[0]
        size = struct.unpack_from('<I', dirdata, off + 120)[0]
        entries.append((name, typ, start, size))

    print("entries:")
    for n, t, s, z in entries[:15]:
        print("   %-24s type=%d start=%d size=%d" % (n, t, s, z))

    minifat = []
    if mf_start <= 0xFFFFFFFA:
        mfd = read_chain(mf_start, 0)
        minifat = [struct.unpack_from('<I', mfd, i * 4)[0] for i in range(len(mfd) // 4)]

    ministry = b''
    if entries and entries[0][1] == 5 and entries[0][3] > 0:
        ministry = read_chain(entries[0][2], entries[0][3])

    def read_mini(start, size):
        out = bytearray()
        sid, g = start, 0
        while sid <= 0xFFFFFFFA and len(out) < size and g < len(minifat) + 1:
            off = sid * mss
            if off + mss > len(ministry):
                break
            out += ministry[off:off + mss]
            if sid >= len(minifat):
                break
            sid = minifat[sid]
            g += 1
        return bytes(out[:size])

    for name, typ, start, size in entries:
        if typ == 2 and name.lower() == 'basicfileinfo':
            data = read_mini(start, size) if size < cutoff else read_chain(start, size)
            print("\n=== BasicFileInfo (%d bytes) ===" % len(data))
            dump_fields(data)
            return data

    print("\nNO BasicFileInfo stream")
    return None


def dump_fields(raw):
    if len(raw) < 64:
        print("too short")
        return
    print("hex[0:96]:", raw[:96].hex())
    for off in (0, 4, 8, 12, 14, 18, 22, 26, 30):
        if off + 4 <= len(raw):
            print("  u32@%-3d = %d" % (off, struct.unpack_from('<i', raw, off)[0]))

    txt = raw.decode('utf-16-le', 'replace')
    printable = ''.join(c if 32 <= ord(c) < 127 else '.' for c in txt)
    print("\nUTF-16 preview:")
    print(printable[:700])


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    dump(sys.argv[1])
