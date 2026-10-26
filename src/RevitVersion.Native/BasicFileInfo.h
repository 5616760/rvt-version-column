// =====================================================================
// BasicFileInfo 语义解析（跨版本）
//
// 为什么不用固定偏移：
//   各 Revit 版本的 BasicFileInfo 字段顺序不同，实测到三种布局：
//     2008 : +14 "20080126_1900"（只有构建号，无版本串）
//     2016 : +14 "Autodesk Revit 2016 (Build: 20150110_1515(x64))"
//     2018 : +14 "Autodesk Revit 2018 (Build: 20170117_1515(x64))"
//     2020 : 长度前缀串序列 + 纯文本 key-value（UTF-16 可能奇偶错位）
//     2023 : 第一个串是保存路径，版本串在第二个位置
//   旧实现按 2021 的 +14/+18/+26 硬编码，在 2023 上读不到构建号。
//
// 现在的策略（三级）：
//   1) 在 UTF-16 文本中按 key 语义匹配：Format: / Build: / Locale / Worksharing
//   2) 匹配 "Autodesk Revit 20XX (Build: ...)" 这种整句形式
//   3) 兜底扫描独立的 20xx token，以及 YYYYMMDD_HHHH 构建号
//
// 关键细节：
//   - 2023 的 "Format: 2023Build: ..." 之间没有分隔符，必须用前瞻而非要求空白。
//   - key-value 段可能从奇数偏移开始（字节错位），需同时试 offset 0 和 1。
//   - 长度前缀单位是「字符数」不是字节数。
// =====================================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <stdint.h>

namespace bfi
{

struct Meta
{
    bool     ok = false;
    int      formatVersion = 0;        // 如 2021
    std::wstring build;                // "20250724_1515(x64)"
    std::wstring locale;               // "CHS" / "ENU" / "ENG"
    std::wstring savedPath;            // 上次保存路径
    std::wstring worksharing;          // "Central" / "Workshared" / ""
};

// ---------------------------------------------------------------------
// 基础工具
// ---------------------------------------------------------------------
inline bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

// 在 [start,end) 内查找子串（区分大小写 ASCII）
inline size_t FindAscii(const std::wstring& s, const wchar_t* needle, size_t start = 0)
{
    size_t n = wcslen(needle);
    if (n == 0 || s.size() < n) return std::wstring::npos;
    for (size_t i = start; i + n <= s.size(); i++)
    {
        size_t k = 0;
        while (k < n && s[i + k] == needle[k]) k++;
        if (k == n) return i;
    }
    return std::wstring::npos;
}

// 不区分大小写的子串查找。
// 需要它的原因：同一批文件里 key 大小写不统一，
// 实测同时存在 "Locale When Saved:" 与 "Locale when saved:"。
inline wchar_t LowerAscii(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
}

inline size_t FindAsciiNoCase(const std::wstring& s, const wchar_t* needle, size_t start = 0)
{
    size_t n = wcslen(needle);
    if (n == 0 || s.size() < n) return std::wstring::npos;
    for (size_t i = start; i + n <= s.size(); i++)
    {
        size_t k = 0;
        while (k < n && LowerAscii(s[i + k]) == LowerAscii(needle[k])) k++;
        if (k == n) return i;
    }
    return std::wstring::npos;
}

// 从 pos 读取连续数字，最多 maxDigits 位
inline int ReadDigits(const std::wstring& s, size_t pos, int maxDigits, std::wstring* captured = nullptr)
{
    int value = 0, count = 0;
    size_t p = pos;
    while (p < s.size() && count < maxDigits && IsDigit(s[p]))
    {
        value = value * 10 + (s[p] - L'0');
        count++;
        p++;
    }
    if (captured)
    {
        captured->clear();
        if (count > 0) captured->assign(s, pos, (size_t)count);
    }
    return count > 0 ? value : -1;
}

// 取到行尾（\r \n 或 NUL）
inline std::wstring ReadLine(const std::wstring& s, size_t pos, size_t maxLen = 400)
{
    std::wstring out;
    while (pos < s.size() && out.size() < maxLen)
    {
        wchar_t c = s[pos];
        if (c == L'\r' || c == L'\n' || c == L'\0') break;
        out.push_back(c);
        pos++;
    }
    return out;
}

inline void Trim(std::wstring& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t')) b++;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t')) e--;
    s = s.substr(b, e - b);
}

// 构建号形如 YYYYMMDD_HHHH 或 YYYYMMDD_HHHH(x64)
inline bool IsBuildToken(const std::wstring& s)
{
    if (s.size() < 13) return false;
    for (int i = 0; i < 8; i++) if (!IsDigit(s[i])) return false;
    if (s[8] != L'_') return false;
    for (int i = 9; i < 13; i++) if (!IsDigit(s[i])) return false;
    return true;
}

// ---------------------------------------------------------------------
// 在文本中提取构建号
// ---------------------------------------------------------------------
inline std::wstring ExtractBuild(const std::wstring& txt)
{
    // 1) "Revit Build: YYYYMMDD_HHHH..."（2008/旧版）
    // 2) "Build: YYYYMMDD_HHHH..."（新版 key-value；注意不能吃掉 "Revit Build:"）
    // 3) "(Build: YYYYMMDD_HHHH...)"（"Autodesk Revit 20XX (Build: ...)"）
    size_t pos = 0;
    while ((pos = FindAscii(txt, L"Build:", pos)) != std::wstring::npos)
    {
        size_t p = pos + 6;
        while (p < txt.size() && txt[p] == L' ') p++;

        std::wstring digits;
        ReadDigits(txt, p, 8, &digits);
        if (digits.size() != 8) { pos += 6; continue; }

        size_t q = p + 8;
        if (q >= txt.size() || txt[q] != L'_') { pos += 6; continue; }

        std::wstring rest;
        ReadDigits(txt, q + 1, 4, &rest);
        if (rest.size() != 4) { pos += 6; continue; }

        // 收集构建号：YYYYMMDD_HHHH 后若紧跟 "(...)" 一并纳入（如 "(x64)"）。
        // 括号必须配平，否则会把外层 "(Build: ...)" 的右括号也吃进来。
        size_t start = p;
        size_t end = q + 5;                       // 已确认 "_HHHH"
        if (end < txt.size() && txt[end] == L'(')
        {
            size_t close = end + 1;
            while (close < txt.size() && (close - end) < 24)
            {
                wchar_t c = txt[close];
                if (c == L'\r' || c == L'\n' || c == L'\0') break;
                if (c == L')') { close++; break; }
                if (c < 0x20 || c > 0x7E) break;
                close++;
            }
            if (close <= txt.size() && close > end + 1 && txt[close - 1] == L')')
                end = close;
        }

        std::wstring out(txt, start, end - start);
        Trim(out);
        if (IsBuildToken(out)) return out;

        pos += 6;
    }
    return std::wstring();
}

// ---------------------------------------------------------------------
// 提取版本号
// ---------------------------------------------------------------------
inline int ExtractFormat(const std::wstring& txt)
{
    // 1) "Format: 2023"（新版；值后可能直接跟 "Build:"，因此只取 4 位数字）
    size_t p = FindAscii(txt, L"Format:");
    if (p != std::wstring::npos)
    {
        size_t q = p + 7;
        while (q < txt.size() && (txt[q] == L' ' || txt[q] == L'\t')) q++;
        std::wstring d;
        int v = ReadDigits(txt, q, 4, &d);
        if (d.size() == 4 && v >= 2000 && v <= 2100) return v;
        // 值可能被 NUL 分隔，尝试宽松匹配
        if (d.size() == 4) return v;
    }

    // 2) "Autodesk Revit 2016 (Build: ...)"
    p = FindAscii(txt, L"Autodesk Revit ");
    if (p != std::wstring::npos)
    {
        size_t q = p + 15;
        std::wstring d;
        int v = ReadDigits(txt, q, 4, &d);
        if (d.size() == 4 && v >= 2000 && v <= 2100) return v;
    }

    // 3) 兜底：扫描独立 20xx token
    for (size_t i = 0; i + 4 <= txt.size(); i++)
    {
        if (txt[i] != L'2' || txt[i + 1] != L'0') continue;
        if (!IsDigit(txt[i + 2]) || !IsDigit(txt[i + 3])) continue;

        // 必须前后不是数字，避免从更长数字里截取
        if (i > 0 && IsDigit(txt[i - 1])) continue;
        if (i + 4 < txt.size() && IsDigit(txt[i + 4])) continue;

        int v = (txt[i] - L'0') * 1000 + (txt[i + 1] - L'0') * 100
              + (txt[i + 2] - L'0') * 10 + (txt[i + 3] - L'0');
        if (v >= 2004 && v <= 2100) return v;
    }
    return 0;
}

// ---------------------------------------------------------------------
// 提取语言 / 工作共享状态
// ---------------------------------------------------------------------
inline void ExtractExtras(const std::wstring& txt, Meta& meta)
{
    // key 大小写在同一目录的不同文件里并不统一（实测 "Locale When Saved"
    // 与 "Locale when saved" 并存），因此统一用不区分大小写匹配。
    size_t p = FindAsciiNoCase(txt, L"Locale when saved:");
    if (p != std::wstring::npos)
    {
        size_t q = p + 18;
        while (q < txt.size() && (txt[q] == L' ' || txt[q] == L'\t')) q++;
        std::wstring loc;
        while (q < txt.size() && loc.size() < 8)
        {
            wchar_t c = txt[q];
            if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'))) break;
            loc.push_back(c);
            q++;
        }
        if (!loc.empty()) meta.locale = loc;
    }

    p = FindAsciiNoCase(txt, L"Worksharing:");
    if (p != std::wstring::npos)
    {
        std::wstring v = ReadLine(txt, p + 12, 40);
        Trim(v);
        if (FindAsciiNoCase(v, L"Central") == 0)
            meta.worksharing = L"Central";
        else if (FindAsciiNoCase(v, L"Not enabled") == 0 || v.empty())
            meta.worksharing.clear();
        else
            meta.worksharing = v;
    }
    else if (FindAsciiNoCase(txt, L"Central Model Path:") != std::wstring::npos)
    {
        // 早期文件没有 Worksharing: 行，用 Central Model Path 是否有内容判断
        size_t q = FindAsciiNoCase(txt, L"Central Model Path:") + 19;
        std::wstring v = ReadLine(txt, q, 300);
        Trim(v);
        if (!v.empty()) meta.worksharing = L"Central";
    }

    p = FindAsciiNoCase(txt, L"Last Save Path:");
    if (p != std::wstring::npos)
        meta.savedPath = ReadLine(txt, p + 15, 400);
}

// ---------------------------------------------------------------------
// 入口：解析 BasicFileInfo 原始字节
// ---------------------------------------------------------------------
inline void Parse(const std::vector<uint8_t>& raw, Meta& meta)
{
    if (raw.size() < 16) return;

    // UTF-16 文本可能从奇数偏移开始（2020 样本实测奇数对齐才读得到 key-value），
    // 因此两种对齐都解析，再按字段择优合并。
    std::wstring txtA, txtB;
    int fmtA = 0, fmtB = 0;
    std::wstring buildA, buildB;

    for (int shift = 0; shift <= 1; shift++)
    {
        size_t bytes = raw.size() - (size_t)shift;
        bytes &= ~(size_t)1;                      // 必须是偶数
        if (bytes < 16) continue;

        std::wstring txt((const wchar_t*)(raw.data() + shift), bytes / 2);
        int f = ExtractFormat(txt);
        std::wstring build = ExtractBuild(txt);

        // 极早期文件（2008 及更早）只有 "Revit Build: YYYYMMDD_HHHH"，
        // 没有 Format 字段，用构建号年份反推格式号。
        if (f == 0 && !build.empty() && build.size() >= 4)
        {
            int y = _wtoi(std::wstring(build, 0, 4).c_str());
            if (y >= 2004 && y <= 2100) f = y;
        }

        if (shift == 0) { txtA = txt; fmtA = f; buildA = build; }
        else            { txtB = txt; fmtB = f; buildB = build; }
    }

    // 版本号：取任一非零值（两路结果应一致；不一致时优先有构建号的一路）
    int fmt = 0;
    std::wstring baseTxt;
    if (fmtA > 0 && !buildA.empty())      { fmt = fmtA; baseTxt = txtA; }
    else if (fmtB > 0 && !buildB.empty()) { fmt = fmtB; baseTxt = txtB; }
    else if (fmtA > 0)                    { fmt = fmtA; baseTxt = txtA; }
    else if (fmtB > 0)                    { fmt = fmtB; baseTxt = txtB; }
    else                                  return;

    meta.formatVersion = fmt;

    // 构建号同理
    if (!buildA.empty())      meta.build = buildA;
    else if (!buildB.empty()) meta.build = buildB;

    // 附加字段：两路都试，取先命中者（错位的那一路通常读不到 key）
    ExtractExtras(baseTxt, meta);
    if (meta.locale.empty())
    {
        const std::wstring& other = (&baseTxt == &txtA) ? txtB : txtA;
        if (!other.empty()) ExtractExtras(other, meta);
    }

    meta.ok = (meta.formatVersion > 0);
}

// ---------------------------------------------------------------------
// 长度前缀字符串序列（供诊断与兜底使用）
// ---------------------------------------------------------------------
inline bool ReadPrefixedString(const std::vector<uint8_t>& raw, size_t pos,
                               std::wstring& out, size_t& next)
{
    if (pos + 4 > raw.size()) return false;
    int32_t chars = *(const int32_t*)(raw.data() + pos);
    if (chars <= 0 || chars > 4096) return false;

    size_t bytes = (size_t)chars * 2;
    if (pos + 4 + bytes > raw.size()) return false;

    out.assign((const wchar_t*)(raw.data() + pos + 4), (size_t)chars);
    next = pos + 4 + bytes;
    return true;
}

} // namespace bfi
