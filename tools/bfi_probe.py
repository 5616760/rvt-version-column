"""在 BasicFileInfo 中定位纯文本 key-value 段，跨版本验证。

用法：
    python tools/bfi_probe.py "file1.rvt" "file2.rfa" ...
"""
import struct
import sys


def read_bfi(path):
    f = open(path, 'rb')
    hdr = f.read(512)
    if hdr[:8] != b'\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1':
        return None, "not CFB"

    ss = 1 << struct.unpack_from('<H', hdr, 30)[0]
    mss = 1 << struct.unpack_from('<H', hdr, 32)[0]
    cutoff = struct.unpack_from('<I', hdr, 56)[0]
    fat_cnt = struct.unpack_from('<i', hdr, 44)[0]
    dir_start = struct.unpack_from('<I', hdr, 48)[0]
    mf_start = struct.unpack_from('<I', hdr, 60)[0]
    difat_start = struct.unpack_from('<I', hdr, 68)[0]
    difat_cnt = struct.unpack_from('<i', hdr, 72)[0]

    def rs(sid):
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
        buf = rs(cur)
        for i in range(ss // 4 - 1):
            sid = struct.unpack_from('<I', buf, i * 4)[0]
            if sid > 0xFFFFFFFA:
                break
            fat_ids.append(sid)
        cur = struct.unpack_from('<I', buf, ss - 4)[0]
        guard += 1

    fat = []
    for sid in fat_ids:
        buf = rs(sid)
        for i in range(ss // 4):
            fat.append(struct.unpack_from('<I', buf, i * 4)[0])

    def rchain(start, size):
        out = bytearray()
        sid, g = start, 0
        while sid <= 0xFFFFFFFA and g < len(fat) + 1:
            if size > 0 and len(out) >= size:
                break
            out += rs(sid)[:size - len(out)] if size > 0 else rs(sid)
            if sid >= len(fat):
                break
            sid = fat[sid]
            g += 1
        return bytes(out)

    dd = rchain(dir_start, 0)
    entries = []
    for off in range(0, len(dd) - 127, 128):
        nlen = struct.unpack_from('<H', dd, off + 64)[0]
        typ = dd[off + 66]
        if typ == 0 or nlen < 2 or nlen > 64:
            continue
        entries.append((dd[off:off + nlen - 2].decode('utf-16-le', 'replace'), typ,
                        struct.unpack_from('<I', dd, off + 116)[0],
                        struct.unpack_from('<I', dd, off + 120)[0]))

    minifat = []
    if mf_start <= 0xFFFFFFFA:
        mfd = rchain(mf_start, 0)
        minifat = [struct.unpack_from('<I', mfd, i * 4)[0] for i in range(len(mfd) // 4)]

    ministry = b''
    if entries and entries[0][1] == 5 and entries[0][3] > 0:
        ministry = rchain(entries[0][2], entries[0][3])

    def rmini(start, size):
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
            return (rmini(start, size) if size < cutoff else rchain(start, size)), None
    return None, "no BasicFileInfo"


def kv_extract(raw):
    """提取 Format: / Build: 等 key-value 行。"""
    txt = raw.decode('utf-16-le', 'replace')
    found = {}
    for key in ('Format', 'Build', 'Last Save Path', 'Worksharing', 'Username',
                'Central Model Path', 'Revit Build', 'Locale', 'Version'):
        idx = txt.find(key + ': ')
        if idx >= 0:
            start = idx + len(key) + 2
            end = start
            while end < len(txt) and txt[end] not in '\r\n' and ord(txt[end]) >= 32:
                end += 1
            found[key] = txt[start:end][:120]
    return found, txt


def main():
    for path in sys.argv[1:]:
        print("=" * 90)
        print(path)
        raw, err = read_bfi(path)
        if err:
            print("  !!", err)
            continue
        print("  BasicFileInfo size =", len(raw))
        print("  head:", raw[:24].hex())

        kv, txt = kv_extract(raw)
        if kv:
            for k, v in kv.items():
                print("  %-20s = %s" % (k, v))
        else:
            print("  [无 key-value 段]")
            printable = ''.join(c if 32 <= ord(c) < 127 else '.' for c in txt)
            print("  preview:", printable[:300])


if __name__ == '__main__':
    main()
