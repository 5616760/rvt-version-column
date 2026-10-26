"""语义解析原型：在 BasicFileInfo 中按内容识别版本/构建号/语言/工作共享。

不依赖固定偏移——各版本字段顺序不同，已实测：
  2008: "Revit Build: 20080126_1900"
  2016: "Autodesk Revit 2016 (Build: 20150110_1515(x64))"
  2018: "Autodesk Revit 2018 (Build: 20170117_1515(x64))"
  2023: "Format: 2023Build: 20250724_1515(x64)"   ← 注意 Format 值后无分隔符

用法：python tools/semantic_probe.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bfi_probe

# --- 版本 ---
RE_FORMAT = re.compile(r'Format:\s*(\d{4})')
RE_PRODUCT = re.compile(r'Autodesk\s+Revit\s+(\d{4})')
RE_REVIT_BUILD = re.compile(r'Revit Build:\s*([0-9]{8}_[0-9]{4}(?:\([^)]*\))?)')
# --- 构建号 ---
RE_BUILD_KV = re.compile(r'(?<!Revit )Build:\s*([0-9]{8}_[0-9]{4}(?:\([^)]*\))?)')
RE_BUILD_PAREN = re.compile(r'\(Build:\s*([0-9]{8}_[0-9]{4}(?:\([^)]*\))?)')
# --- 其他 ---
RE_LOCALE = re.compile(r'Locale When Saved:\s*([A-Za-z]{2,8})')
RE_WORKSET = re.compile(r'Worksharing:\s*([^\r\n\x00]{0,40})')
RE_SAVED = re.compile(r'Last Save Path:\s*([^\r\n\x00]{1,400})')

SAMPLES = [
    (r"C:\Program Files\Autodesk\Batch Print for Revit 2018\BatchPrintInitial.rvt", 2008),
    (r"C:\Program Files\Autodesk\Revit 2018\Samples\rac_basic_sample_project.rvt", 2018),
    (r"C:\Program Files\Autodesk\Revit 2018\Samples\rme_advanced_sample_project.rvt", 2018),
    (r"C:\Program Files\Autodesk\Revit 2020\Samples\rac_basic_sample_project.rvt", 2020),
    (r"C:\Program Files\Autodesk\Revit 2020\Samples\Commercial_Sample_Project.rvt", 2020),
    (r"C:\Program Files\Autodesk\FormIt Converter For Revit 2018\FormItConversionAddon\Resources\2016\Library\Metric\Desk.rfa", 2016),
    (r"C:\Program Files\Autodesk\FormIt Converter For Revit 2018\FormItConversionAddon\Resources\2016\Templates\Imperial\imperial.rte", 2016),
    (r"C:\Program Files\Autodesk\FormIt Converter For Revit 2018\FormItConversionAddon\Resources\2016\Templates\Imperial\Casework.rft", 2016),
    (r"G:\DATA\Work\12T5A\合模\1 Archi\RVT\T5_MBC_CS-KPF-T5A-PLR-ARC-M3-00005.rvt", 2023),
    (r"G:\DATA\Work\12T5A\合模\1 Archi\RVT\T5_MBC_CS-KPF-T5A-ZZ-ARC-M3-00003.rvt", 2023),
    (r"G:\DATA\Work\12T5A\合模\1 Archi\RVT\T5_MBC_CS-KPF-T5A-ZZ-ARC-M3-00013.rvt", 2023),
    (r"G:\DATA\Work\12T5A\合模\1 Archi\RVT\T5_MBC_CS-KPF-T5A-ZZ-ARC-M3-00101.rvt", 2023),
]


def parse(raw):
    txt = raw.decode('utf-16-le', 'replace')
    out = {}

    fmt = None
    m = RE_FORMAT.search(txt)
    if m:
        fmt = int(m.group(1))
    if fmt is None:
        m = RE_PRODUCT.search(txt)
        if m:
            fmt = int(m.group(1))

    # 构建号
    build = None
    for rx in (RE_BUILD_KV, RE_BUILD_PAREN, RE_REVIT_BUILD):
        m = rx.search(txt)
        if m:
            build = m.group(1)
            break

    if fmt is None:
        # 兜底：扫描独立 20xx token
        for m in re.finditer(r'\b(20\d{2})\b', txt):
            v = int(m.group(1))
            if 2004 <= v <= 2100:
                fmt = v
                break

    # 极早期文件（2008 及更早）只有 "Revit Build: YYYYMMDD_xxxx"，无 Format 字段。
    # 此时用构建号年份推断，并映射到格式号（2008 文件即 2008 格式）。
    if fmt is None and build:
        y = int(build[:4])
        if 2004 <= y <= 2100:
            fmt = y

    out['format'] = fmt
    out['build'] = build

    m = RE_LOCALE.search(txt)
    out['locale'] = m.group(1) if m else None

    m = RE_WORKSET.search(txt)
    if m:
        v = m.group(1).strip()
        out['worksharing'] = 'Central' if v.lower().startswith('central') else (
            '' if v.lower().startswith('not enabled') else v)
    else:
        out['worksharing'] = None

    m = RE_SAVED.search(txt)
    out['savedPath'] = m.group(1) if m else None
    out['nostream'] = None
    return out


def main():
    ok = bad = 0
    print("%-46s %-6s %-6s %-24s %-6s %s" % ("file", "expect", "got", "build", "locale", "ws"))
    print("-" * 130)
    for path, expect in SAMPLES:
        name = os.path.basename(path)
        if not os.path.exists(path):
            print("%-46s (missing)" % name)
            continue
        raw, err = bfi_probe.read_bfi(path)
        if not raw:
            print("%-46s !! %s" % (name, err))
            bad += 1
            continue
        r = parse(raw)
        good = (r['format'] == expect)
        ok += 1 if good else 0
        bad += 0 if good else 1
        print("%-46s %-6s %-6s %-24s %-6s %s  %s" % (
            name[:46], expect, r['format'], (r['build'] or '-')[:24],
            r['locale'] or '-', r['worksharing'] or '-', '' if good else '  <-- MISMATCH'))
    print("-" * 130)
    print("pass %d / %d" % (ok, ok + bad))


if __name__ == '__main__':
    main()
