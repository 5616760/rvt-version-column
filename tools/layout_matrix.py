"""跨版本验证 BasicFileInfo 字段布局（诊断用）。

用法：python tools/layout_matrix.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bfi_probe

CANDIDATES = [
    r"C:\Program Files\Autodesk\Batch Print for Revit 2025\BatchPrintInitial.rvt",
    r"C:\Program Files\Autodesk\Batch Print for Revit 2024\BatchPrintInitial.rvt",
    r"C:\Program Files\Autodesk\Batch Print for Revit 2023\BatchPrintInitial.rvt",
    r"C:\Program Files\Autodesk\Batch Print for Revit 2020\BatchPrintInitial.rvt",
    r"C:\Program Files\Autodesk\Batch Print for Revit 2018\BatchPrintInitial.rvt",
    r"C:\Program Files\Autodesk\Revit 2018\Samples\rac_basic_sample_project.rvt",
    r"C:\Program Files\Autodesk\FormIt Converter For Revit 2018\FormItConversionAddon\Resources\2016\Library\Metric\Desk.rfa",
    r"C:\Program Files\Autodesk\FormIt Converter For Revit 2018\FormItConversionAddon\Resources\2016\Templates\Imperial\imperial.rte",
    r"C:\Program Files\Autodesk\FormIt Converter For Revit 2018\FormItConversionAddon\Resources\2016\Templates\Imperial\Casework.rft",
]


def walk(raw, start, maxf=10):
    out, pos = [], start
    for _ in range(maxf):
        if pos + 4 > len(raw):
            break
        c = struct.unpack_from('<i', raw, pos)[0]
        if c <= 0 or c > 3000 or pos + 4 + c * 2 > len(raw):
            break
        s = raw[pos + 4:pos + 4 + c * 2].decode('utf-16-le', 'replace')
        if any(ord(ch) < 32 and ch not in '\r\n\t' for ch in s):
            break
        out.append((pos, c, s))
        pos += 4 + c * 2
    return out


def main():
    for p in CANDIDATES:
        label = os.path.basename(p)
        print("=" * 88)
        if not os.path.exists(p):
            print(label, "  (不存在)")
            continue
        raw, err = bfi_probe.read_bfi(p)
        if not raw:
            print(label, "  !!", err)
            continue
        print("%s  size=%d  u32@0=%d" % (label, len(raw), struct.unpack_from('<i', raw, 0)[0]))
        for st in (10, 14, 18):
            seq = walk(raw, st)
            if not seq:
                continue
            print("  start=%d -> %d fields" % (st, len(seq)))
            for pos, c, s in seq[:5]:
                print("     @%-5d len=%-4d %r" % (pos, c, s[:70]))


if __name__ == '__main__':
    main()
