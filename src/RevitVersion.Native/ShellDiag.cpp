// =====================================================================
// Shell 注册诊断 / 列布局工具
//
// 为什么需要它：
//   1. reg.exe 在本机被安全策略列入黑名单，PowerShell 访问 HKLM 也被
//      沙箱拦截，只有自编译的进程能读到真实注册表状态。
//   2. 判断「列头出现但值空白」必须区分两种情况：
//        - Shell 根本没把我们的 handler 算进去
//        - handler 被调用但返回空
//      HandlerTest.cpp 是进程内直连 DLL，绕过了注册，证明不了前者。
//      本工具走 SHGetPropertyStoreFromParsingName —— 与资源管理器完全
//      相同的代码路径，能给出决定性结论。
//
// 用法：
//   rvtvdiag --check                    检查全部注册项
//   rvtvdiag --store <file.rvt>         用 Shell 的路径读属性并打印
//   rvtvdiag --dump-cols                列出各 FolderType 的 ColumnList
//   rvtvdiag --add-col <{GUID}>         把 Revit Version 加进该类型的默认列
//   rvtvdiag --del-col <{GUID}>         从该类型的默认列里移除
//   rvtvdiag --list-foldertypes         只列出 GUID + 本地化名称
// =====================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propvarutil.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "PropKeys.h"

// ---- 与 PropertyHandler.cpp / Setup.cpp 保持一致 ----
static const wchar_t* kClsid   = L"{5AD3E9FC-A720-47F6-8DFA-9AACD3286E01}";
static const wchar_t* kShellEx = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";
static const wchar_t* kExts[]  = { L".rvt", L".rfa", L".rte", L".rft" };

static const wchar_t* kFolderTypes =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FolderTypes";
static const wchar_t* kPropHandlers =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers";

// 我们想默认出现在列里的属性 canonical name
static const wchar_t* kOurCanonical = L"RvtVersion.ProductYear";

// ---------------------------------------------------------------- 输出
// SetConsoleOutputCP 只影响真控制台；输出被重定向到管道/文件时
// wprintf 会按 ANSI 代码页转，中文全变问号。
// 统一自己转 UTF-8 字节直接写 stdout，两种情形都正确。
static void Out(const wchar_t* fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return;
    std::string s((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, &s[0], n, nullptr, nullptr);
    fwrite(s.data(), 1, s.size(), stdout);
    fflush(stdout);
}

// ---------------------------------------------------------------- 注册表读
static bool RegGetSz(HKEY root, const std::wstring& sub, const wchar_t* name,
                     std::wstring& out, REGSAM extra = 0)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | extra, &k) != ERROR_SUCCESS)
        return false;

    wchar_t buf[1024] = { 0 };
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(k, name, nullptr, &type, (BYTE*)buf, &cb);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    buf[cb / sizeof(wchar_t)] = 0;
    out = buf;
    return true;
}

static bool SubKeyNames(HKEY root, const std::wstring& sub, std::vector<std::wstring>& out)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return false;

    DWORD i = 0;
    wchar_t name[512];
    for (;;)
    {
        DWORD cch = 512;
        LONG rc = RegEnumKeyExW(k, i++, name, &cch, nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) break;
        out.push_back(name);
    }
    RegCloseKey(k);
    return true;
}

// ---------------------------------------------------------------- --check
static int Check()
{
    Out(L"\n=== 1. COM 类注册 ===\n");

    // HKCR 是 HKLM\SOFTWARE\Classes 与 HKCU\SOFTWARE\Classes 的合并视图，
    // 所以 hkey=HKEY_CLASSES_ROOT 时子键必须是 "CLSID\{...}"，
    // 不能再拼 "SOFTWARE\Classes\CLSID\..."（那是直接走 HKLM 时的写法）。
    std::wstring clsidKey = std::wstring(L"CLSID\\") + kClsid;
    std::wstring inproc, thread;
    bool hasClsid = RegGetSz(HKEY_CLASSES_ROOT, clsidKey + L"\\InprocServer32", nullptr, inproc);
    RegGetSz(HKEY_CLASSES_ROOT, clsidKey + L"\\InprocServer32", L"ThreadingModel", thread);

    Out(L"  CLSID 键          : %ls\n", hasClsid ? L"OK" : L"<缺失>");
    if (hasClsid)
    {
        Out(L"  InprocServer32    : %ls\n", inproc.c_str());
        Out(L"  ThreadingModel    : %ls\n", thread.empty() ? L"<未设>" : thread.c_str());

        DWORD attr = GetFileAttributesW(inproc.c_str());
        bool exist = (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
        Out(L"  DLL 文件存在      : %ls\n", exist ? L"OK" : L"<找不到>");
    }

    Out(L"\n=== 2. 每扩展名的 handler 注册 ===\n");
    Out(L"  %-6ls %-14ls %-14ls %ls\n", L"ext", L"ShellEx", L"PropHandlers", L"ProgID");

    int shellExOk = 0, propHOk = 0;
    for (size_t i = 0; i < _countof(kExts); i++)
    {
        std::wstring ext = kExts[i];

        std::wstring se;
        bool seOk = RegGetSz(HKEY_CLASSES_ROOT, ext + L"\\ShellEx\\" + kShellEx, nullptr, se) &&
                    _wcsicmp(se.c_str(), kClsid) == 0;

        std::wstring ph;
        bool phOk = RegGetSz(HKEY_LOCAL_MACHINE, std::wstring(kPropHandlers) + L"\\" + ext,
                             nullptr, ph, KEY_WOW64_64KEY) &&
                    _wcsicmp(ph.c_str(), kClsid) == 0;

        std::wstring progId;
        RegGetSz(HKEY_CLASSES_ROOT, ext, nullptr, progId);

        Out(L"  %-6ls %-14ls %-14ls %ls\n", ext.c_str(),
            seOk ? L"OK" : L"MISSING",
            phOk ? L"OK" : L"MISSING",
            progId.empty() ? L"<无>" : progId.c_str());

        if (seOk) shellExOk++;
        if (phOk) propHOk++;
    }

    Out(L"\n  汇总: ShellEx %d/4, PropertyHandlers %d/4\n", shellExOk, propHOk);
    if (propHOk == 0)
        Out(L"  [!] PropertyHandlers 全缺 —— 这是 Shell 取 IPropertyStore 的主路径，\n"
            L"      缺了就会出现「列头在、值全空」。\n");

    Out(L"\n=== 3. 属性 schema ===\n");
    {
        int found = 0;
        IPropertySystem* sys = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_PropertySystem, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_IPropertySystem, (void**)&sys)) && sys)
        {
            for (size_t i = 0; i < kPropCount; i++)
            {
                IPropertyDescription* d = nullptr;
                if (SUCCEEDED(sys->GetPropertyDescription(kProps[i].key,
                                                          IID_IPropertyDescription,
                                                          (void**)&d)) && d)
                {
                    LPWSTR canon = nullptr;
                    d->GetCanonicalName(&canon);
                    if (i == 1 && canon)
                        Out(L"  ProductYear canonical = %ls\n", canon);
                    if (canon) CoTaskMemFree(canon);
                    d->Release();
                    found++;
                }
            }
            sys->Release();
        }
        Out(L"  已注册属性: %d / %zu\n", found, kPropCount);
    }

    Out(L"\n");
    bool allOk = hasClsid && shellExOk == 4 && propHOk == 4;
    Out(L"结论: %ls\n", allOk ? L"注册齐全" : L"注册不完整（见上）");
    return allOk ? 0 : 2;
}

// ---------------------------------------------------------------- --store
static void DumpKeys(IPropertyStore* store, const char* label)
{
    DWORD n = 0;
    if (FAILED(store->GetCount(&n)))
    {
        Out(L"  [%hs] GetCount 失败\n", label);
        return;
    }

    Out(L"  [%hs] 属性总数 = %lu\n", label, n);

    int ours = 0;
    for (DWORD i = 0; i < n; i++)
    {
        PROPERTYKEY k;
        if (FAILED(store->GetAt(i, &k))) continue;
        if (!IsEqualGUID(k.fmtid, FMTID_RvtVersion)) continue;

        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(k, &pv)))
        {
            PWSTR s = nullptr;
            std::wstring val;
            if (SUCCEEDED(PropVariantToStringAlloc(pv, &s)) && s)
            {
                val = s;
                CoTaskMemFree(s);
            }
            Out(L"      pid=%-2lu  %ls\n", k.pid, val.empty() ? L"<空>" : val.c_str());
            PropVariantClear(&pv);
        }
        ours++;
    }

    if (ours == 0)
        Out(L"      我们的属性一个都没有\n");
    else
        Out(L"      我们的属性 %d 个\n", ours);
}

static int StoreTest(const wchar_t* path)
{
    Out(L"\n=== Shell 路径读取 ===\n");
    Out(L"  原始 : %ls\n", path);

    // Git Bash / MSYS 会把参数里的反斜杠转成正斜杠。GetFileAttributesW 能忍，
    // SHParseDisplayName 不能（直接 E_INVALIDARG）。统一归一化回反斜杠。
    std::wstring fixed(path);
    for (auto& c : fixed) if (c == L'/') c = L'\\';

    Out(L"  归一 : %ls\n", fixed.c_str());

    DWORD attr = GetFileAttributesW(fixed.c_str());
    Out(L"  属性 : 0x%08lX %ls\n", (unsigned long)attr,
        attr == INVALID_FILE_ATTRIBUTES ? L"<文件不存在！>" : L"OK");

    PIDLIST_ABSOLUTE pidl = nullptr;
    SFGAOF sfgaof = 0;
    HRESULT hr = SHParseDisplayName(fixed.c_str(), nullptr, &pidl, 0, &sfgaof);
    Out(L"  SHParseDisplayName hr=0x%08lX\n", (unsigned long)hr);

    if (FAILED(hr) || !pidl)
    {
        // 兜底：IShellItem2 路线，对长路径与特殊字符更宽容。
        // OpenPropertyStore 在 IShellItem2 上，不在 IShellItem 上。
        IShellItem2* item = nullptr;
        hr = SHCreateItemFromParsingName(fixed.c_str(), nullptr, IID_IShellItem2, (void**)&item);
        Out(L"  SHCreateItemFromParsingName(IID_IShellItem2) hr=0x%08lX\n", (unsigned long)hr);
        if (SUCCEEDED(hr) && item)
        {
            // IShellItem2 没有 OpenPropertyStore（那是 IShellItem 上通过
            // BindToHandler 做的事），它直接暴露 GetPropertyStore。
            IPropertyStore* s = nullptr;
            hr = item->GetPropertyStore(GPS_DEFAULT, IID_IPropertyStore, (void**)&s);
            Out(L"  GetPropertyStore hr=0x%08lX\n", (unsigned long)hr);
            if (SUCCEEDED(hr) && s)
            {
                DumpKeys(s, "IShellItem2/GPS_DEFAULT");
                PROPVARIANT pv;
                PropVariantInit(&pv);
                bool got = false;
                if (SUCCEEDED(s->GetValue(PKEY_RvtProductYear, &pv)))
                {
                    PWSTR str = nullptr;
                    if (SUCCEEDED(PropVariantToStringAlloc(pv, &str)) && str)
                    {
                        Out(L"      >> ProductYear = \"%ls\"\n", str);
                        got = (*str != 0);
                        CoTaskMemFree(str);
                    }
                }
                PropVariantClear(&pv);
                s->Release();
                item->Release();
                Out(L"\n  结论: %ls\n", got ? L"能拿到值" : L"拿不到值");
                return got ? 0 : 2;
            }
            item->Release();
        }
        Out(L"[x] 无法解析路径\n");
        return 2;
    }

    struct Mode { GETPROPERTYSTOREFLAGS f; const char* label; };
    const Mode modes[] = {
        { GPS_DEFAULT,                                        "GPS_DEFAULT" },
        { GPS_HANDLERPROPERTIESONLY,                          "HANDLERPROPERTIESONLY" },
        { (GETPROPERTYSTOREFLAGS)(GPS_OPENSLOWITEM | GPS_BESTEFFORT),
                                                              "OPENSLOWITEM|BESTEFFORT" },
    };

    bool anyOurs = false;

    for (size_t m = 0; m < _countof(modes); m++)
    {
        IPropertyStore* store = nullptr;
        hr = SHGetPropertyStoreFromIDList(pidl, modes[m].f, IID_IPropertyStore, (void**)&store);
        Out(L"\n  flags=0x%02lX %-24hs hr=0x%08lX\n",
            (unsigned long)modes[m].f, modes[m].label, (unsigned long)hr);
        if (FAILED(hr) || !store) continue;

        DumpKeys(store, modes[m].label);

        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(PKEY_RvtProductYear, &pv)))
        {
            PWSTR s = nullptr;
            if (SUCCEEDED(PropVariantToStringAlloc(pv, &s)) && s)
            {
                Out(L"      >> ProductYear = \"%ls\"\n", s);
                if (*s) anyOurs = true;
                CoTaskMemFree(s);
            }
        }
        PropVariantClear(&pv);

        store->Release();
    }

    CoTaskMemFree(pidl);

    Out(L"\n  结论: %ls\n",
        anyOurs ? L"Shell 能拿到 Revit Version 值 —— handler 已生效"
                : L"Shell 拿不到值 —— handler 未被采用，见 --check");
    return anyOurs ? 0 : 2;
}

// ---------------------------------------------------------------- FolderTypes
struct FtRow
{
    std::wstring guid;
    std::wstring name;
    int          colCount = 0;
    bool         hasOurs  = false;
};

// 递归列出某个键的完整结构，用于摸清 FolderTypes 的真实布局
static void DumpTree(HKEY root, const std::wstring& sub, int depth, int maxDepth)
{
    if (depth > maxDepth) return;

    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return;

    std::wstring indent((size_t)(depth * 2), L' ');

    // 值
    DWORD idx = 0;
    wchar_t name[256];
    for (;;)
    {
        DWORD cch = 256;
        DWORD type = 0;
        if (RegEnumValueW(k, idx++, name, &cch, nullptr, &type, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        std::wstring v;
        if (type == REG_SZ || type == REG_EXPAND_SZ)
        {
            RegGetSz(root, sub, name[0] ? name : nullptr, v, KEY_WOW64_64KEY);
            // ColumnList 这类值要看完整内容，否则改不对格式
            if (v.size() > 100 && wcsstr(name, L"ColumnList") == nullptr)
                v = v.substr(0, 100) + L"...";
        }
        else
        {
            v = L"<非字符串 type=" + std::to_wstring(type) + L">";
        }
        Out(L"%ls  [%ls] = %ls\n", indent.c_str(),
            name[0] ? name : L"(default)", v.c_str());
    }

    // 子键
    std::vector<std::wstring> subs;
    if (SubKeyNames(root, sub, subs))
    {
        for (const auto& s : subs)
        {
            Out(L"%ls  + %ls\n", indent.c_str(), s.c_str());
            DumpTree(root, sub + L"\\" + s, depth + 1, maxDepth);
        }
    }

    RegCloseKey(k);
}

static int TreeOf(const wchar_t* guid)
{
    std::wstring sub = std::wstring(kFolderTypes) + L"\\" + guid;
    Out(L"\n=== %ls ===\n", sub.c_str());
    DumpTree(HKEY_LOCAL_MACHINE, sub, 0, 3);
    return 0;
}

// 查哪些扩展名 / ProgID 绑定了 FolderType，摸清「按类型指定列布局」的机制
static int FindFolderTypeBindings()
{
    Out(L"\n=== 扩展名 / ProgID 上的 FolderType 绑定 ===\n\n");

    int hits = 0;

    // 遍历 HKCR 下所有 .ext，看有没有 FolderType 值
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"", 0, KEY_READ, &hk) != ERROR_SUCCESS)
    {
        Out(L"[x] 打不开 HKCR\n");
        return 1;
    }

    DWORD i = 0;
    wchar_t name[512];
    for (;;)
    {
        DWORD cch = 512;
        if (RegEnumKeyExW(hk, i++, name, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        if (name[0] != L'.') continue;   // 只看扩展名

        std::wstring ft;
        if (RegGetSz(HKEY_CLASSES_ROOT, name, L"FolderType", ft))
        {
            Out(L"  %-20ls FolderType = %ls\n", name, ft.c_str());
            hits++;
        }
    }
    RegCloseKey(hk);

    // ProgID 上的 FolderType
    Out(L"\n=== ProgID 上的 FolderType（抽样已知类型）===\n");
    const wchar_t* progIds[] = {
        L"Revit.Project", L"Revit.Family", L"Revit.Template", L"Revit.FamilyTemplate",
        L"txtfile", L"Word.Document.8", L"AcadDrawing", L"image/jpeg",
    };
    for (const auto* p : progIds)
    {
        std::wstring ft;
        if (RegGetSz(HKEY_CLASSES_ROOT, p, L"FolderType", ft))
        {
            Out(L"  %-24ls FolderType = %ls\n", p, ft.c_str());
            hits++;
        }
        else
        {
            Out(L"  %-24ls <无 FolderType>\n", p);
        }
    }

    Out(L"\n  共找到 %d 处绑定\n", hits);
    Out(L"\n  说明: 若系统里几乎没有扩展名带 FolderType，说明这个机制\n");
    Out(L"        在本机不通用，自动勾选列要走 FolderTypes 默认列布局。\n");
    return 0;
}

// 用户级视图状态。Windows 10 的真实位置是
// HKCU\SOFTWARE\Classes\Local Settings\Software\Microsoft\Windows\Shell\Bags，
// 不是 HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\Shell\Bags。
// AllFolders 子键就是「应用到所有文件夹」写入的地方。
// 验证 HKCU 是否有可写、且被 Shell 读取的 FolderTypes / 视图路径。
//
// 动机：HKLM 的 FolderTypes 归 NT AUTHORITY\SYSTEM 所有，管理员也写不进。
// 如果 Shell 会合并 HKCU 的对应位置，就能免提权实现「默认显示某列」。
// 这是整套自动列方案的可行性前提，必须先测。
static int ProbeUserPaths()
{
    Out(L"\n=== HKCU 候选路径可写性 ===\n\n");

    struct Probe { const wchar_t* path; const wchar_t* label; };
    const Probe probes[] = {
        { L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FolderTypes",
          L"Explorer\\FolderTypes" },
        { L"SOFTWARE\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\Bags",
          L"Local Settings\\...\\Shell\\Bags" },
        { L"SOFTWARE\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\BagMRU",
          L"Local Settings\\...\\Shell\\BagMRU" },
        { L"SOFTWARE\\Classes\\.rvt", L"Classes\\.rvt" },
        { L"SOFTWARE\\Classes\\Revit.Project", L"Classes\\Revit.Project" },
    };

    for (const auto& p : probes)
    {
        HKEY k = nullptr;
        LONG rr = RegOpenKeyExW(HKEY_CURRENT_USER, p.path, 0, KEY_READ, &k);
        bool exists = (rr == ERROR_SUCCESS);
        if (k) RegCloseKey(k);

        k = nullptr;
        LONG wr = RegOpenKeyExW(HKEY_CURRENT_USER, p.path, 0, KEY_SET_VALUE, &k);
        bool writable = (wr == ERROR_SUCCESS);
        if (k) RegCloseKey(k);

        // 不存在时，看能否创建
        bool creatable = false;
        if (!exists)
        {
            k = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, p.path, 0, nullptr, 0,
                                KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS)
            {
                creatable = true;
                RegCloseKey(k);
                RegDeleteKeyW(HKEY_CURRENT_USER, p.path);   // 清掉探测痕迹
            }
        }

        Out(L"  %-40ls 存在=%-3ls 可写=%-3ls 可创建=%-3ls\n",
            p.label,
            exists ? L"YES" : L"no",
            writable ? L"YES" : L"no",
            creatable ? L"YES" : L"no");
    }

    // 已有 FolderTypes 条目？
    std::vector<std::wstring> subs;
    if (SubKeyNames(HKEY_CURRENT_USER,
                    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FolderTypes",
                    subs))
    {
        Out(L"\n  HKCU FolderTypes 下已有 %zu 个子键:\n", subs.size());
        for (const auto& s : subs) Out(L"      %ls\n", s.c_str());
    }
    else
    {
        Out(L"\n  HKCU 下没有 FolderTypes 键（Shell 通常只读 HKLM）\n");
    }

    return 0;
}

// ---------------------------------------------------------------- 递归复制
// 把一个键整棵复制到另一个根下。用于把 HKLM 的 FolderType 定义搬到 HKCU，
// 再在副本上改 ColumnList —— 这样无论 Shell 对 HKCU 是「合并」还是
// 「整块替代」，都不会丢掉 Name / LogicalViewMode / SortByList 等其他设置。
static bool CopyKeyTree(HKEY srcRoot, const std::wstring& srcSub,
                        HKEY dstRoot, const std::wstring& dstSub)
{
    HKEY src = nullptr;
    if (RegOpenKeyExW(srcRoot, srcSub.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &src)
        != ERROR_SUCCESS)
        return false;

    HKEY dst = nullptr;
    if (RegCreateKeyExW(dstRoot, dstSub.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                        nullptr, &dst, nullptr) != ERROR_SUCCESS)
    {
        RegCloseKey(src);
        return false;
    }

    // 复制值
    DWORD idx = 0;
    wchar_t name[512];
    for (;;)
    {
        DWORD cch = 512;
        DWORD type = 0, cb = 0;
        if (RegEnumValueW(src, idx++, name, &cch, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS)
            break;

        std::vector<BYTE> buf(cb + 8);
        DWORD cb2 = cb;
        if (RegQueryValueExW(src, name, nullptr, &type, buf.data(), &cb2) != ERROR_SUCCESS)
            continue;

        // 跳过空的 (default)
        RegSetValueExW(dst, name, 0, type, buf.data(), cb2);
    }

    // 递归子键
    std::vector<std::wstring> subs;
    idx = 0;
    for (;;)
    {
        DWORD cch = 512;
        if (RegEnumKeyExW(src, idx++, name, &cch, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS)
            break;
        subs.push_back(name);
    }

    for (const auto& s : subs)
        CopyKeyTree(srcRoot, srcSub + L"\\" + s, dstRoot, dstSub + L"\\" + s);

    RegCloseKey(dst);
    RegCloseKey(src);
    return true;
}

// 用户级默认列：写 HKCU，不需要提权，也不碰 SYSTEM 拥有的 HKLM。
//
// 原理：Shell 读取 FolderType 定义时会看 HKCU 下的同名路径
// （HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\FolderTypes）。
// 实测 HKCU 下已存在 {4DCAFE13-...}（Pictures.SearchResults）的覆盖项，
// 说明这条路径确实被采用。写之前先整棵复制 HKLM 定义，避免丢设置。
static int EditColUser(const wchar_t* guid, bool add)
{
    const std::wstring base =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FolderTypes\\";
    const std::wstring hklmSub = base + guid;
    const std::wstring hkcuSub = base + guid;

    Out(L"\n=== 用户级默认列: %ls ===\n", guid);

    // 是否已有 HKCU 覆盖
    HKEY test = nullptr;
    bool existed = RegOpenKeyExW(HKEY_CURRENT_USER, hkcuSub.c_str(), 0, KEY_READ, &test)
                   == ERROR_SUCCESS;
    if (test) RegCloseKey(test);
    Out(L"  HKCU 已有覆盖: %ls\n", existed ? L"YES" : L"no");

    if (!existed)
    {
        if (!CopyKeyTree(HKEY_LOCAL_MACHINE, hklmSub, HKEY_CURRENT_USER, hkcuSub))
        {
            Out(L"[x] 从 HKLM 复制定义失败\n");
            return 1;
        }
        Out(L"  已从 HKLM 完整复制定义到 HKCU\n");
    }

    // 找 TopViews 下的视图 GUID（通常是全零，但复制后以实际为准）
    const std::wstring tvBase = hkcuSub + L"\\TopViews";
    std::vector<std::wstring> views;
    if (!SubKeyNames(HKEY_CURRENT_USER, tvBase, views) || views.empty())
    {
        Out(L"[x] HKCU 下没有 TopViews 子键\n");
        return 1;
    }
    Out(L"  TopViews: %zu 个视图，用 %ls\n", views.size(), views[0].c_str());

    const std::wstring viewPath = tvBase + L"\\" + views[0];

    std::wstring cur;
    if (!RegGetSz(HKEY_CURRENT_USER, viewPath, L"ColumnList", cur))
    {
        Out(L"[x] 读不到 ColumnList\n");
        return 1;
    }
    Out(L"  原值: %ls\n", cur.c_str());

    if (cur.find(L"prop:") == 0)
        cur = cur.substr(5);

    // 拆 / 过滤 / 追加
    std::vector<std::wstring> kept;
    {
        std::wstring rest = cur;
        size_t p = 0;
        while ((p = rest.find(L';')) != std::wstring::npos)
        {
            std::wstring it = rest.substr(0, p);
            if (it.find(kOurCanonical) == std::wstring::npos) kept.push_back(it);
            rest = rest.substr(p + 1);
        }
        if (!rest.empty() && rest.find(kOurCanonical) == std::wstring::npos)
            kept.push_back(rest);
    }

    if (add)
        kept.push_back(L"0(14)" + std::wstring(kOurCanonical));

    std::wstring out = L"prop:";
    for (size_t i = 0; i < kept.size(); i++)
    {
        if (i) out += L";";
        out += kept[i];
    }

    HKEY k = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, viewPath.c_str(), 0, KEY_SET_VALUE, &k);
    if (rc != ERROR_SUCCESS)
    {
        Out(L"[x] 打开键失败 rc=%ld\n", rc);
        return 1;
    }
    rc = RegSetValueExW(k, L"ColumnList", 0, REG_SZ, (const BYTE*)out.c_str(),
                        (DWORD)((out.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);

    if (rc != ERROR_SUCCESS)
    {
        Out(L"[x] 写入失败 rc=%ld\n", rc);
        return 1;
    }

    Out(L"  新值: %ls\n", out.c_str());

    std::wstring back;
    if (RegGetSz(HKEY_CURRENT_USER, viewPath, L"ColumnList", back))
        Out(L"  回读: %ls\n", back.c_str());

    return 0;
}

// ---------------------------------------------------------------- BagMRU
// 已有文件夹的列布局存在 Bag 里，会覆盖 FolderTypes 默认值。
// 要让「已打开过的文件夹」也自动出现新列，必须清掉它对应的 Bag 记录，
// 让 Shell 重新从 FolderTypes 继承。
//
// BagMRU 是树：每个节点有 MRUListEx（DWORD 序列）和数字子键，
// 数字子键的值是 SHITEMID 列表（即文件夹名，UTF-16），
// 节点的 NodeSlot 指向 Bags\Shell\<编号>。
static std::wstring ReadBagMruItemName(HKEY node, const wchar_t* valueName)
{
    // 数字值 = 一段 SHITEMID 序列。第一个 item 是文件夹名，
    // 结构为 WORD cb + 数据（含结尾 \0）。
    HKEY k = nullptr;
    (void)k;
    DWORD cb = 0;
    if (RegQueryValueExW(node, valueName, nullptr, nullptr, nullptr, &cb) != ERROR_SUCCESS || cb == 0)
        return std::wstring();

    std::vector<BYTE> buf(cb + 8);
    DWORD cb2 = cb;
    if (RegQueryValueExW(node, valueName, nullptr, nullptr, buf.data(), &cb2) != ERROR_SUCCESS)
        return std::wstring();

    if (cb2 < 2) return std::wstring();
    WORD itemLen = *(WORD*)buf.data();
    if (itemLen < 2 || (DWORD)itemLen + 2 > cb2) return std::wstring();

    // 跳过起始的 2 字节长度，后面是 UTF-16（含结尾 \0）
    const wchar_t* s = (const wchar_t*)(buf.data() + 2);
    size_t maxChars = (itemLen - 2) / sizeof(wchar_t);
    std::wstring out(s, maxChars);
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

static DWORD ReadDword(HKEY root, const std::wstring& sub, const wchar_t* name, DWORD deflt = 0)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return deflt;
    DWORD v = deflt, cb = sizeof(v), type = 0;
    RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&v, &cb);
    RegCloseKey(k);
    return v;
}

// 递归遍历 BagMRU，找出包含指定扩展名文件的文件夹对应的 Bag 编号
static void WalkBagMru(HKEY node, const std::wstring& pathSoFar,
                       std::vector<std::pair<std::wstring, DWORD>>& hits,
                       int depth)
{
    if (depth > 24) return;

    // 当前节点对应的 Bag
    DWORD cb = 0;
    DWORD nodeSlot = 0;
    {
        DWORD v = 0, c = sizeof(v), type = 0;
        if (RegQueryValueExW(node, L"NodeSlot", nullptr, &type, (BYTE*)&v, &c) == ERROR_SUCCESS)
            nodeSlot = v;
    }
    (void)cb;

    if (nodeSlot != 0 && !pathSoFar.empty())
    {
        // 该文件夹是否含我们关心的文件
        WIN32_FIND_DATAW fd;
        std::wstring pattern = pathSoFar + L"\\*.rvt";
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        bool hasRvt = (h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) FindClose(h);

        if (!hasRvt)
        {
            pattern = pathSoFar + L"\\*.rfa";
            h = FindFirstFileW(pattern.c_str(), &fd);
            hasRvt = (h != INVALID_HANDLE_VALUE);
            if (h != INVALID_HANDLE_VALUE) FindClose(h);
        }

        if (hasRvt) hits.push_back({ pathSoFar, nodeSlot });
    }

    // 枚举子键（数字名）
    std::vector<std::wstring> subs;
    DWORD idx = 0;
    wchar_t name[512];
    for (;;)
    {
        DWORD cch = 512;
        if (RegEnumKeyExW(node, idx++, name, &cch, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS)
            break;
        // 只处理纯数字子键
        bool numeric = name[0] != 0;
        for (const wchar_t* p = name; *p; p++)
            if (*p < L'0' || *p > L'9') { numeric = false; break; }
        if (numeric) subs.push_back(name);
    }

    for (const auto& s : subs)
    {
        HKEY child = nullptr;
        if (RegOpenKeyExW(node, s.c_str(), 0, KEY_READ, &child) != ERROR_SUCCESS) continue;

        std::wstring itemName = ReadBagMruItemName(child, s.c_str());
        // 跳过虚拟项（"::{...}" 之类）
        std::wstring childPath;
        if (!itemName.empty() && itemName[0] != L':')
            childPath = pathSoFar + L"\\" + itemName;

        WalkBagMru(child, childPath, hits, depth + 1);
        RegCloseKey(child);
    }
}

static int ListRvtBags()
{
    Out(L"\n=== 含 Revit 文件的文件夹视图缓存 ===\n\n");

    const std::wstring bagMru =
        L"SOFTWARE\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\BagMRU";

    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, bagMru.c_str(), 0, KEY_READ, &root) != ERROR_SUCCESS)
    {
        Out(L"[x] 打不开 BagMRU\n");
        return 1;
    }

    // 从各盘符根开始
    std::vector<std::pair<std::wstring, DWORD>> hits;

    // 根节点的子项是盘符
    std::vector<std::wstring> drives;
    DWORD idx = 0;
    wchar_t name[512];
    for (;;)
    {
        DWORD cch = 512;
        if (RegEnumKeyExW(root, idx++, name, &cch, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS)
            break;
        bool numeric = name[0] != 0;
        for (const wchar_t* p = name; *p; p++)
            if (*p < L'0' || *p > L'9') { numeric = false; break; }
        if (numeric) drives.push_back(name);
    }

    for (const auto& d : drives)
    {
        HKEY child = nullptr;
        if (RegOpenKeyExW(root, d.c_str(), 0, KEY_READ, &child) != ERROR_SUCCESS) continue;
        std::wstring itemName = ReadBagMruItemName(child, d.c_str());
        if (!itemName.empty() && itemName.back() != L':')
            itemName += L":";           // 盘符补冒号
        WalkBagMru(child, itemName, hits, 1);
        RegCloseKey(child);
    }
    RegCloseKey(root);

    Out(L"  命中 %zu 个含 Revit 的文件夹（有视图缓存）\n\n", hits.size());
    for (const auto& h : hits)
        Out(L"      Bag %-6lu %ls\n", h.second, h.first.c_str());

    return 0;
}

// 原始导出 BagMRU 某一层的结构，用于反推 SHITEMID 编码
static int DumpBagMruRaw(const wchar_t* sub)
{
    const std::wstring base =
        L"SOFTWARE\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\BagMRU";
    std::wstring path = base;
    if (sub && *sub) path += std::wstring(L"\\") + sub;

    Out(L"\n=== %ls ===\n", path.c_str());

    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
    {
        Out(L"[x] 打不开\n");
        return 1;
    }

    DWORD idx = 0;
    wchar_t name[512];
    for (;;)
    {
        DWORD cch = 512, type = 0, cb = 0;
        if (RegEnumValueW(k, idx++, name, &cch, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS)
            break;

        Out(L"  值 [%ls] type=%lu cb=%lu\n", name[0] ? name : L"(default)", type, cb);

        if (cb > 0 && cb <= 256)
        {
            std::vector<BYTE> buf(cb + 8);
            DWORD cb2 = cb;
            if (RegQueryValueExW(k, name, nullptr, &type, buf.data(), &cb2) == ERROR_SUCCESS)
            {
                Out(L"      hex : ");
                for (DWORD i = 0; i < cb2 && i < 64; i++) Out(L"%02X ", buf[i]);
                if (cb2 > 64) Out(L"...");
                Out(L"\n");

                // 尝试当 UTF-16 解读
                if (cb2 >= 4)
                {
                    WORD first = *(WORD*)buf.data();
                    Out(L"      首 WORD = %u (0x%04X)\n", first, first);
                    // 从偏移 2 开始按 UTF-16 读
                    if (cb2 > 4)
                    {
                        std::wstring asStr((const wchar_t*)(buf.data() + 2),
                                           (cb2 - 2) / sizeof(wchar_t));
                        while (!asStr.empty() && asStr.back() == L'\0') asStr.pop_back();
                        Out(L"      @+2 utf16 = [%ls]\n", asStr.c_str());
                    }
                }
            }
        }
    }

    std::vector<std::wstring> subs;
    idx = 0;
    for (;;)
    {
        DWORD cch = 512;
        if (RegEnumKeyExW(k, idx++, name, &cch, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS)
            break;
        subs.push_back(name);
    }
    Out(L"\n  子键 %zu 个:", subs.size());
    for (size_t i = 0; i < subs.size() && i < 20; i++) Out(L" %ls", subs[i].c_str());
    Out(L"\n");

    RegCloseKey(k);
    return 0;
}

static int DumpUserBags()
{
    const wchar_t* base =
        L"SOFTWARE\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell";

    Out(L"\n=== HKCU\\%ls ===\n", base);

    std::vector<std::wstring> subs;
    if (!SubKeyNames(HKEY_CURRENT_USER, base, subs))
    {
        Out(L"[x] 打不开\n");
        return 1;
    }
    for (const auto& s : subs) Out(L"  + %ls\n", s.c_str());

    std::wstring bagsPath = std::wstring(base) + L"\\Bags";
    std::vector<std::wstring> bags;
    if (!SubKeyNames(HKEY_CURRENT_USER, bagsPath, bags))
    {
        Out(L"\n[x] Bags 不存在: %ls\n", bagsPath.c_str());
        return 1;
    }

    Out(L"\n=== Bags (%zu 个) ===\n", bags.size());
    for (const auto& b : bags)
    {
        Out(L"\n  --- Bag %ls ---\n", b.c_str());

        // 每个 Bag 下通常有 Shell / ShellNoRoam 子键，再下面才是 {FolderTypeGUID}
        const wchar_t* kinds[] = { L"ShellNoRoam", L"Shell" };
        for (const auto* kind : kinds)
        {
            std::wstring kp = bagsPath + L"\\" + b + L"\\" + kind;
            std::vector<std::wstring> views;
            if (!SubKeyNames(HKEY_CURRENT_USER, kp, views)) continue;

            for (const auto& v : views)
            {
                std::wstring vp = kp + L"\\" + v;
                Out(L"      [%ls] %ls\n", kind, v.c_str());

                // 列信息：ColInfo / Col<N> / GroupByKey / Mode / LogicalViewMode
                HKEY k = nullptr;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, vp.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
                    continue;

                DWORD idx = 0;
                wchar_t vn[256];
                for (;;)
                {
                    DWORD cch = 256;
                    DWORD type = 0;
                    if (RegEnumValueW(k, idx++, vn, &cch, nullptr, &type, nullptr, nullptr)
                        != ERROR_SUCCESS) break;

                    if (type == REG_SZ || type == REG_EXPAND_SZ)
                    {
                        std::wstring s;
                        if (RegGetSz(HKEY_CURRENT_USER, vp, vn, s))
                        {
                            if (s.size() > 90) s = s.substr(0, 90) + L"...";
                            Out(L"          [%ls] = %ls\n", vn, s.c_str());
                        }
                    }
                    else if (type == REG_BINARY)
                    {
                        DWORD cb = 0;
                        RegQueryValueExW(k, vn, nullptr, nullptr, nullptr, &cb);
                        if (cb <= 12)
                        {
                            BYTE buf[16] = { 0 };
                            DWORD cb2 = cb;
                            if (RegQueryValueExW(k, vn, nullptr, nullptr, buf, &cb2) == ERROR_SUCCESS)
                            {
                                Out(L"          [%ls] = bin(%lu)", vn, cb);
                                for (DWORD bi = 0; bi < cb2 && bi < 8; bi++)
                                    Out(L" %02X", buf[bi]);
                                Out(L"\n");
                            }
                        }
                        else
                        {
                            Out(L"          [%ls] = bin(%lu bytes)\n", vn, cb);
                        }
                    }
                }
                RegCloseKey(k);
            }
        }
    }
    return 0;
}

// 查键的所有者与当前进程能否写入。
// FolderTypes 受系统保护时，管理员也会拿到 ERROR_ACCESS_DENIED(5)，
// 需要判断是 DACL 问题（可接管所有权）还是结构上不可写。
static void CheckKeyAccess(HKEY root, const std::wstring& sub)
{
    Out(L"\n=== 权限检查: %ls ===\n", sub.c_str());

    // 1) 读尝试
    HKEY k = nullptr;
    LONG rc = RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k);
    Out(L"  读 (KEY_READ)                  rc=%ld %ls\n", rc, rc == 0 ? L"OK" : L"FAIL");
    if (k) RegCloseKey(k);

    // 2) 写尝试
    k = nullptr;
    rc = RegOpenKeyExW(root, sub.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &k);
    Out(L"  写 (KEY_SET_VALUE)             rc=%ld %ls\n", rc, rc == 0 ? L"OK" : L"FAIL");
    if (k) RegCloseKey(k);

    // 3) 所有者
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (RegGetKeySecurity(root, OWNER_SECURITY_INFORMATION, nullptr, nullptr) == ERROR_SUCCESS)
    {
        // 先问大小
    }

    DWORD cb = 0;
    rc = RegGetKeySecurity(root, OWNER_SECURITY_INFORMATION, nullptr, &cb);
    if (rc == ERROR_INSUFFICIENT_BUFFER && cb > 0)
    {
        sd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, cb);
        if (sd)
        {
            if (RegGetKeySecurity(root, OWNER_SECURITY_INFORMATION, sd, &cb) == ERROR_SUCCESS)
            {
                PSID owner = nullptr;
                BOOL deflt = FALSE;
                if (GetSecurityDescriptorOwner(sd, &owner, &deflt) && owner)
                {
                    wchar_t name[256] = { 0 };
                    wchar_t domain[256] = { 0 };
                    DWORD nlen = 256, dlen = 256;
                    SID_NAME_USE use;
                    if (LookupAccountSidW(nullptr, owner, name, &nlen, domain, &dlen, &use))
                        Out(L"  所有者                        %ls\\%ls\n", domain, name);
                    else
                        Out(L"  所有者                        <无法解析, err=%lu>\n", GetLastError());
                }
            }
            LocalFree(sd);
        }
    }

    // 4) 当前是否管理员
    BOOL admin = FALSE;
    PSID g = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &g))
    {
        CheckTokenMembership(nullptr, g, &admin);
        FreeSid(g);
    }
    Out(L"  当前进程是管理员              %ls\n", admin ? L"YES" : L"NO");
}

static int AccessCheck(const wchar_t* guid)
{
    CheckKeyAccess(HKEY_LOCAL_MACHINE,
                   std::wstring(kFolderTypes) + L"\\" + guid);
    CheckKeyAccess(HKEY_LOCAL_MACHINE,
                   std::wstring(kFolderTypes) + L"\\" + guid +
                   L"\\TopViews\\{00000000-0000-0000-0000-000000000000}");
    return 0;
}

static int DumpCols(bool listOnly)
{
    std::vector<std::wstring> guids;
    if (!SubKeyNames(HKEY_LOCAL_MACHINE, kFolderTypes, guids))
    {
        Out(L"[x] 打不开 FolderTypes\n");
        return 1;
    }

    std::vector<FtRow> rows;
    for (const auto& g : guids)
    {
        FtRow r;
        r.guid = g;

        std::wstring ld;
        if (RegGetSz(HKEY_LOCAL_MACHINE, std::wstring(kFolderTypes) + L"\\" + g,
                     L"LocalizedDisplayName", ld, KEY_WOW64_64KEY))
            r.name = ld;

        std::wstring colPath = std::wstring(kFolderTypes) + L"\\" + g + L"\\ColumnList";
        HKEY ck = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, colPath.c_str(), 0,
                          KEY_READ | KEY_WOW64_64KEY, &ck) == ERROR_SUCCESS)
        {
            DWORD idx = 0;
            wchar_t name[64];
            for (;;)
            {
                DWORD cch = 64;
                if (RegEnumValueW(ck, idx++, name, &cch, nullptr, nullptr, nullptr, nullptr)
                    != ERROR_SUCCESS)
                    break;
                r.colCount++;

                std::wstring v;
                if (RegGetSz(HKEY_LOCAL_MACHINE, colPath, name, v, KEY_WOW64_64KEY))
                {
                    if (wcsstr(v.c_str(), L"RvtVersion") != nullptr)
                        r.hasOurs = true;
                }
            }
            RegCloseKey(ck);
        }

        rows.push_back(r);
    }

    Out(L"\n=== FolderTypes (%zu 个) ===\n\n", rows.size());
    Out(L"  %-40s %-6s %-8s %s\n", L"GUID", L"列数", L"含我们", L"LocalizedDisplayName");
    for (const auto& r : rows)
    {
        if (r.colCount == 0 && !listOnly) continue;
        Out(L"  %-40ls %-6d %-8ls %ls\n", r.guid.c_str(), r.colCount,
                r.hasOurs ? L"YES" : L"-", r.name.empty() ? L"-" : r.name.c_str());
    }

    if (listOnly) return 0;

    // 明细：把有列的类型的实际列表打出来
    Out(L"\n=== ColumnList 明细 ===\n");
    for (const auto& r : rows)
    {
        if (r.colCount == 0) continue;
        Out(L"\n  %ls  [%ls]\n", r.guid.c_str(), r.name.empty() ? L"-" : r.name.c_str());

        std::wstring colPath = std::wstring(kFolderTypes) + L"\\" + r.guid + L"\\ColumnList";
        HKEY ck = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, colPath.c_str(), 0,
                          KEY_READ | KEY_WOW64_64KEY, &ck) != ERROR_SUCCESS) continue;

        std::vector<std::pair<std::wstring, std::wstring>> vals;
        DWORD idx = 0;
        wchar_t name[64];
        for (;;)
        {
            DWORD cch = 64;
            if (RegEnumValueW(ck, idx++, name, &cch, nullptr, nullptr, nullptr, nullptr)
                != ERROR_SUCCESS)
                break;
            std::wstring v;
            if (RegGetSz(HKEY_LOCAL_MACHINE, colPath, name, v, KEY_WOW64_64KEY))
                vals.push_back({ name, v });
        }
        RegCloseKey(ck);

        for (const auto& p : vals)
            Out(L"      [%ls] = %ls\n", p.first.c_str(), p.second.c_str());
    }

    return 0;
}

// 在 ColumnList 里增删我们的属性。
// 实测本机（Win10 22H2）结构：
//   FolderTypes\{GUID}\TopViews\{全零GUID}\[ColumnList] = REG_SZ
//   语法 "prop:0(34)Name;1Other" —— 0 显示 / 1 隐藏，(宽度) 可选。
// 这个值名就叫 ColumnList，不是子键。
static int EditCol(const wchar_t* guid, bool add)
{
    const std::wstring sub =
        std::wstring(kFolderTypes) + L"\\" + guid +
        L"\\TopViews\\{00000000-0000-0000-0000-000000000000}";

    Out(L"\n=== %ls默认列: %ls ===\n", add ? L"写入" : L"撤销", guid);
    Out(L"  键: %ls\n", sub.c_str());

    std::wstring cur;
    if (!RegGetSz(HKEY_LOCAL_MACHINE, sub, L"ColumnList", cur, KEY_WOW64_64KEY))
    {
        Out(L"[x] 读不到 ColumnList（需要管理员权限，或键路径不对）\n");
        return 1;
    }
    Out(L"  原值: %ls\n", cur.c_str());

    // "prop:" 只在整串开头出现一次，后续列项不能再带
    if (cur.compare(0, 5, L"prop:") == 0)
        cur = cur.substr(5);

    // 拆列项
    std::vector<std::wstring> items;
    {
        std::wstring rest = cur;
        size_t p = 0;
        while ((p = rest.find(L';')) != std::wstring::npos)
        {
            items.push_back(rest.substr(0, p));
            rest = rest.substr(p + 1);
        }
        if (!rest.empty()) items.push_back(rest);
    }

    // 去掉已存在的同名项
    std::vector<std::wstring> kept;
    for (const auto& it : items)
        if (it.find(kOurCanonical) == std::wstring::npos)
            kept.push_back(it);

    if (add)
        kept.push_back(L"0(14)" + std::wstring(kOurCanonical));

    std::wstring out = L"prop:";
    for (size_t i = 0; i < kept.size(); i++)
    {
        if (i) out += L";";
        out += kept[i];
    }

    // 直接用 RegSetValueExW 写，避免 RegSetStr 对权限标志的处理差异
    HKEY k = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0,
                            KEY_SET_VALUE | KEY_WOW64_64KEY, &k);
    if (rc != ERROR_SUCCESS)
    {
        Out(L"[x] 打开键失败 rc=%ld（需要管理员权限）\n", rc);
        return 1;
    }

    rc = RegSetValueExW(k, L"ColumnList", 0, REG_SZ, (const BYTE*)out.c_str(),
                        (DWORD)((out.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);

    if (rc != ERROR_SUCCESS)
    {
        Out(L"[x] 写入失败 rc=%ld\n", rc);
        return 1;
    }

    Out(L"  新值: %ls\n", out.c_str());

    // 回读
    std::wstring back;
    if (RegGetSz(HKEY_LOCAL_MACHINE, sub, L"ColumnList", back, KEY_WOW64_64KEY))
        Out(L"  回读: %ls\n", back.c_str());

    return 0;
}

// ---------------------------------------------------------------- 真实视图探针
// 前面所有命令读的都是「注册表里写了什么」——那是输入，不是 Shell 的最终判断。
// 判断某文件夹实际会显示哪些列，唯一可靠的办法是让 Shell 自己算一遍：
// 用 IExplorerBrowser 在本进程内拉起一个真实的 Explorer 视图（窗口不可见），
// 再通过 IFolderView2 -> IColumnManager 问它「可见列有哪些」。
// 这条路径与资源管理器窗口完全一致，结论可以直接当作实机行为。
//
// 关键点：
//   - IExplorerBrowser 需要真实 HWND，用 WS_POPUP 不在屏幕上显示；
//   - 必须 SetOptions(EBO_SHOWFRAMES) 才会建 view；
//   - GetCurrentView(IID_IShellView) 拿到 IShellView 后 QI IFolderView2，
//     IFolderView2 再 QI IColumnManager（有些实现只在 IShellView2 上给）。

struct ViewColumnsResult
{
    bool  ok = false;
    DWORD hr = 0;
    std::vector<std::wstring> visible;   // 可见列的 canonical name
    std::vector<std::wstring> all;       // 全部列
    bool  hasOursVisible = false;
};

static ViewColumnsResult ProbeFolderColumns(HWND hwndParent, const std::wstring& path,
                                            bool noPersist = true)
{
    ViewColumnsResult r;

    IExplorerBrowser* eb = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ExplorerBrowser, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IExplorerBrowser,
                                  (void**)&eb);
    if (FAILED(hr) || !eb)
    {
        r.hr = (DWORD)hr;
        return r;
    }

    RECT rc = { 0, 0, 1200, 800 };
    FOLDERSETTINGS fs = {};
    fs.ViewMode  = FVM_DETAILS;       // 详细信息视图，这才有列
    fs.fFlags    = FWF_AUTOARRANGE;

    hr = eb->Initialize(hwndParent, &rc, &fs);
    if (FAILED(hr))
    {
        r.hr = (DWORD)hr;
        eb->Release();
        return r;
    }

    // noPersist=true 时带 EBO_NOPERSISTVIEWSTATE：
    //   好处是探针本身不污染用户的 Bag；
    //   坏处是它同时也可能让 Shell 不读已保存的视图状态 —— 会掩盖「已落盘」的事实。
    // 需要验证持久化时必须传 false。
    eb->SetOptions(noPersist
                   ? (EXPLORER_BROWSER_OPTIONS)(EBO_SHOWFRAMES | EBO_NOPERSISTVIEWSTATE)
                   : (EXPLORER_BROWSER_OPTIONS)(EBO_SHOWFRAMES));

    PIDLIST_ABSOLUTE pidl = nullptr;
    SFGAOF sfgaof = 0;
    hr = SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, &sfgaof);
    if (FAILED(hr) || !pidl)
    {
        r.hr = (DWORD)hr;
        eb->Destroy();
        eb->Release();
        return r;
    }

    hr = eb->BrowseToIDList(pidl, SBSP_ABSOLUTE);
    CoTaskMemFree(pidl);
    if (FAILED(hr))
    {
        r.hr = (DWORD)hr;
        eb->Destroy();
        eb->Release();
        return r;
    }

    IShellView* sv = nullptr;
    hr = eb->GetCurrentView(IID_IShellView, (void**)&sv);
    if (FAILED(hr) || !sv)
    {
        r.hr = (DWORD)hr;
        eb->Destroy();
        eb->Release();
        return r;
    }

    IFolderView2* fv = nullptr;
    hr = sv->QueryInterface(IID_IFolderView2, (void**)&fv);
    if (FAILED(hr) || !fv)
    {
        r.hr = (DWORD)hr;
        sv->Release();
        eb->Destroy();
        eb->Release();
        return r;
    }

    IColumnManager* cm = nullptr;
    hr = fv->QueryInterface(IID_IColumnManager, (void**)&cm);
    if (FAILED(hr) || !cm)
    {
        r.hr = (DWORD)hr;
        fv->Release();
        sv->Release();
        eb->Destroy();
        eb->Release();
        return r;
    }

    auto collect = [&](CM_ENUM_FLAGS flags, std::vector<std::wstring>& sink)
    {
        UINT n = 0;
        if (FAILED(cm->GetColumnCount(flags, &n)) || n == 0) return;
        std::vector<PROPERTYKEY> keys(n);
        if (FAILED(cm->GetColumns(flags, keys.data(), n))) return;
        for (UINT i = 0; i < n; i++)
        {
            PWSTR nm = nullptr;
            std::wstring name;
            if (SUCCEEDED(PSGetNameFromPropertyKey(keys[i], &nm)) && nm)
            {
                name = nm;
                CoTaskMemFree(nm);
            }
            else
            {
                wchar_t buf[64];
                swprintf(buf, 64, L"{%08lX-...},%lu",
                         (unsigned long)keys[i].fmtid.Data1,
                         (unsigned long)keys[i].pid);
                name = buf;
            }
            if (IsEqualGUID(keys[i].fmtid, FMTID_RvtVersion))
                r.hasOursVisible = (flags == CM_ENUM_VISIBLE) ? true : r.hasOursVisible;
            sink.push_back(name);
        }
    };

    collect(CM_ENUM_VISIBLE, r.visible);
    collect(CM_ENUM_ALL,     r.all);

    r.ok = true;
    r.hr = 0;

    cm->Release();
    fv->Release();
    sv->Release();
    eb->Destroy();
    eb->Release();
    return r;
}

// --view-cols <folder> [more folders...]
// 打印每个文件夹「实际会显示的列」，并标出我们在不在里面。
static int ProbeViewColumns(int argc, wchar_t** argv)
{
    Out(L"\n=== 真实 Explorer 视图的可见列（Shell 最终判断）===\n");

    // --view-cols --persist <folder...>
    // 带 --persist 表示用「会读写入过的视图状态」的方式建视图，
    // 这时复用用户自己的视图缓存，才能验证 SetColumns 是否真的落盘。
    bool persist = (argc >= 3 && wcscmp(argv[2], L"--persist") == 0);
    int  start   = persist ? 3 : 2;
    if (persist) Out(L"  模式: 读取已保存的视图状态（可验证持久化）\n");

    // 不可见父窗口：FVM_DETAILS 需要真实 HWND，但不需要用户看见
    HWND parent = CreateWindowExW(0, L"STATIC", L"rvtvprobe", WS_POPUP,
                                  0, 0, 8, 8, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (!parent)
    {
        Out(L"[x] 创建宿主窗口失败 err=%lu\n", GetLastError());
        return 1;
    }

    int rc = 0;
    for (int i = start; i < argc; i++)
    {
        std::wstring p(argv[i]);
        for (auto& c : p) if (c == L'/') c = L'\\';

        Out(L"\n  文件夹: %ls\n", p.c_str());

        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            Out(L"    [x] 不存在或不是文件夹\n");
            rc = 2;
            continue;
        }

        ViewColumnsResult v = ProbeFolderColumns(parent, p, !persist);
        if (!v.ok)
        {
            Out(L"    [x] 取视图失败 hr=0x%08lX\n", (unsigned long)v.hr);
            rc = 2;
            continue;
        }

        Out(L"    可见列 %zu 个:\n", v.visible.size());
        for (size_t k = 0; k < v.visible.size(); k++)
            Out(L"      %2zu. %ls\n", k + 1, v.visible[k].c_str());

        Out(L"    全部列 %zu 个\n", v.all.size());

        // 我们的列在「全部」里但不在「可见」里，说明列已注册只是没勾选；
        // 两边都没有，说明 FolderType 没被认到（列压根不在候选集里）。
        bool inAll = false;
        for (const auto& s : v.all)
            if (s == kOurCanonical) inAll = true;

        Out(L"    我们的列: %ls\n",
            v.hasOursVisible ? L"【可见】"
            : inAll          ? L"在候选集里但未勾选"
                             : L"完全不在候选集");
    }

    DestroyWindow(parent);
    return rc;
}

// --set-cols <folder> add|remove
// 关键实验：能否用 IColumnManager::SetColumns 改某个文件夹的列，并让它持久化。
// 如果可行，就能做到「按文件夹」而不是「全局」显示这一列 —— 这是条件化的前提。
static int SetFolderColumns(const wchar_t* pathArg, bool add)
{
    std::wstring p(pathArg);
    for (auto& c : p) if (c == L'/') c = L'\\';

    Out(L"\n=== 按文件夹改列: %ls  (%ls) ===\n", p.c_str(), add ? L"add" : L"remove");

    HWND parent = CreateWindowExW(0, L"STATIC", L"rvtvprobe", WS_POPUP,
                                  0, 0, 8, 8, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (!parent) { Out(L"[x] 宿主窗口失败\n"); return 1; }

    IExplorerBrowser* eb = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IExplorerBrowser, (void**)&eb);
    if (FAILED(hr)) { Out(L"[x] 建浏览器失败 hr=0x%08lX\n", (unsigned long)hr); DestroyWindow(parent); return 1; }

    RECT rc = { 0, 0, 1200, 800 };
    FOLDERSETTINGS fs = {};
    fs.ViewMode = FVM_DETAILS;
    eb->Initialize(parent, &rc, &fs);
    // 关键：带 EBO_NOPERSISTVIEWSTATE 的话 Shell 不会把视图状态存进 Bag，
    // 我们的修改就白改了。这里必须去掉它，让 Shell 正常持久化。
    eb->SetOptions(EBO_SHOWFRAMES);

    PIDLIST_ABSOLUTE pidl = nullptr;
    SFGAOF sf = 0;
    hr = SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, &sf);
    if (FAILED(hr) || !pidl) { Out(L"[x] 解析路径失败\n"); eb->Destroy(); eb->Release(); DestroyWindow(parent); return 1; }
    eb->BrowseToIDList(pidl, SBSP_ABSOLUTE);
    CoTaskMemFree(pidl);

    IShellView* sv = nullptr;
    IFolderView2* fv = nullptr;
    IColumnManager* cm = nullptr;
    if (FAILED(eb->GetCurrentView(IID_IShellView, (void**)&sv)) || !sv)
    { Out(L"[x] 拿不到 view\n"); goto cleanup; }
    if (FAILED(sv->QueryInterface(IID_IFolderView2, (void**)&fv)) || !fv)
    { Out(L"[x] 拿不到 IFolderView2\n"); goto cleanup; }
    if (FAILED(fv->QueryInterface(IID_IColumnManager, (void**)&cm)) || !cm)
    { Out(L"[x] 拿不到 IColumnManager\n"); goto cleanup; }

    {
        UINT nVis = 0;
        if (SUCCEEDED(cm->GetColumnCount(CM_ENUM_VISIBLE, &nVis)))
        {
            std::vector<PROPERTYKEY> vis(nVis);
            if (nVis && SUCCEEDED(cm->GetColumns(CM_ENUM_VISIBLE, vis.data(), nVis)))
            {
                Out(L"  改前可见列 %u 个\n", nVis);

                bool present = false;
                for (auto& k : vis)
                    if (IsEqualGUID(k.fmtid, FMTID_RvtVersion) && k.pid == PID_RVT_PRODUCT_YEAR)
                        present = true;

                bool want = add;
                if (present == want)
                {
                    Out(L"  已经是目标状态，不用改\n");
                }
                else if (add)
                {
                    vis.push_back(PKEY_RvtProductYear);
                    hr = cm->SetColumns(vis.data(), (UINT)vis.size());
                    Out(L"  SetColumns(追加) hr=0x%08lX\n", (unsigned long)hr);
                }
                else
                {
                    std::vector<PROPERTYKEY> without;
                    for (auto& k : vis)
                        if (!(IsEqualGUID(k.fmtid, FMTID_RvtVersion) && k.pid == PID_RVT_PRODUCT_YEAR))
                            without.push_back(k);
                    hr = cm->SetColumns(without.data(), (UINT)without.size());
                    Out(L"  SetColumns(移除) hr=0x%08lX\n", (unsigned long)hr);
                }
            }
        }
    }

cleanup:
    if (cm) cm->Release();
    if (fv) fv->Release();
    if (sv) sv->Release();
    eb->Destroy();
    eb->Release();
    DestroyWindow(parent);

    // 立刻用全新浏览器复查，看是不是真的落盘了
    Out(L"\n  --- 新开视图复查 ---\n");
    HWND p2 = CreateWindowExW(0, L"STATIC", L"rvtvprobe", WS_POPUP, 0, 0, 8, 8,
                              nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ViewColumnsResult v = ProbeFolderColumns(p2, p);
    DestroyWindow(p2);
    if (!v.ok) { Out(L"  [x] 复查失败\n"); return 1; }
    Out(L"  可见列 %zu 个:", v.visible.size());
    for (const auto& s : v.visible) Out(L" %ls", s.c_str());
    Out(L"\n  我们的列: %ls\n", v.hasOursVisible ? L"【可见 → 持久化成功】" : L"未可见");
    return v.hasOursVisible == add ? 0 : 2;
}

// --real-windows [add|remove]
// 枚举「真实资源管理器窗口」，打印每个窗口当前显示的列。
// 这是条件化方案的技术前提：只有拿到真实窗口的 view，
// SetColumns 才会写进 Shell 的 Bag（自己 new 一个 IExplorerBrowser
// 用的是它自己的 property bag，改完不落盘 —— 已实测）。
static std::wstring WindowFolderPath(IShellBrowser* sb)
{
    IShellView* sv = nullptr;
    if (FAILED(sb->QueryActiveShellView(&sv)) || !sv) return L"";

    IFolderView2* fv = nullptr;
    std::wstring out;
    if (SUCCEEDED(sv->QueryInterface(IID_IFolderView2, (void**)&fv)) && fv)
    {
        IPersistFolder2* pf = nullptr;
        if (SUCCEEDED(fv->GetFolder(IID_IPersistFolder2, (void**)&pf)) && pf)
        {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(pf->GetCurFolder(&pidl)) && pidl)
            {
                wchar_t buf[MAX_PATH * 2] = {};
                if (SHGetPathFromIDListW(pidl, buf)) out = buf;
                CoTaskMemFree(pidl);
            }
            pf->Release();
        }
        fv->Release();
    }
    sv->Release();
    return out;
}

// --real-windows [add|remove|force-add|force-remove]
//   add/remove       只处理「含 .rvt 才加、不含才删」的条件语义
//   force-add       无视内容，所有窗口都加（用于测持久化）
//   force-remove    无视内容，所有窗口都删
static int RealWindows(const wchar_t* action)
{
    Out(L"\n=== 真实资源管理器窗口 ===\n");

    IShellWindows* sw = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                  IID_IShellWindows, (void**)&sw);
    if (FAILED(hr) || !sw)
    {
        Out(L"[x] 拿不到 IShellWindows hr=0x%08lX\n", (unsigned long)hr);
        return 1;
    }

    long count = 0;
    sw->get_Count(&count);
    Out(L"  窗口数 = %ld\n", count);

    int rvtFolders = 0, changed = 0;

    for (long i = 0; i < count; i++)
    {
        VARIANT vi;
        VariantInit(&vi);
        vi.vt = VT_I4;
        vi.lVal = i;

        IDispatch* disp = nullptr;
        hr = sw->Item(vi, &disp);
        if (FAILED(hr) || !disp)
        {
            Out(L"\n  [%ld] Item 失败 hr=0x%08lX\n", i, (unsigned long)hr);
            VariantClear(&vi);
            continue;
        }
        VariantClear(&vi);

        // IShellBrowser 不能直接从 IDispatch QI 到（实测 E_NOINTERFACE）。
        // 正确路径：QI IServiceProvider，再 QueryService(SID_STopLevelBrowser)。
        IShellBrowser* sb = nullptr;
        IServiceProvider* sp = nullptr;
        hr = disp->QueryInterface(IID_IServiceProvider, (void**)&sp);
        if (SUCCEEDED(hr) && sp)
        {
            hr = sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, (void**)&sb);
            sp->Release();
        }
        else
        {
            Out(L"\n  [%ld] 无 IServiceProvider hr=0x%08lX\n", i, (unsigned long)hr);
        }

        if (FAILED(hr) || !sb)
        {
            Out(L"\n  [%ld] 拿不到 IShellBrowser hr=0x%08lX\n", i, (unsigned long)hr);
            disp->Release();
            continue;
        }
        {
            std::wstring folder = WindowFolderPath(sb);
            if (folder.empty())
            {
                Out(L"\n  [%ld] <非文件系统文件夹>\n", i);
            }
            else
            {
                // 这个文件夹里有 Revit 文件吗？
                bool hasRvt = false;
                WIN32_FIND_DATAW fd;
                std::wstring pat = folder + L"\\*.rvt";
                HANDLE h = FindFirstFileW(pat.c_str(), &fd);
                if (h != INVALID_HANDLE_VALUE)
                {
                    hasRvt = true;
                    FindClose(h);
                }
                if (hasRvt) rvtFolders++;

                Out(L"\n  [%ld] %ls\n", i, folder.c_str());
                Out(L"       含 .rvt: %ls\n", hasRvt ? L"是" : L"否");

                // 打印可见列
                IShellView* sv = nullptr;
                if (SUCCEEDED(sb->QueryActiveShellView(&sv)) && sv)
                {
                    IFolderView2* fv = nullptr;
                    if (SUCCEEDED(sv->QueryInterface(IID_IFolderView2, (void**)&fv)) && fv)
                    {
                        IColumnManager* cm = nullptr;
                        if (SUCCEEDED(fv->QueryInterface(IID_IColumnManager, (void**)&cm)) && cm)
                        {
                            UINT n = 0;
                            if (SUCCEEDED(cm->GetColumnCount(CM_ENUM_VISIBLE, &n)))
                            {
                                std::vector<PROPERTYKEY> keys(n);
                                if (n && SUCCEEDED(cm->GetColumns(CM_ENUM_VISIBLE, keys.data(), n)))
                                {
                                    bool present = false;
                                    Out(L"       可见列:");
                                    for (UINT k = 0; k < n; k++)
                                    {
                                        PWSTR nm = nullptr;
                                        if (SUCCEEDED(PSGetNameFromPropertyKey(keys[k], &nm)) && nm)
                                        {
                                            Out(L" %ls", nm);
                                            if (wcscmp(nm, kOurCanonical) == 0) present = true;
                                            CoTaskMemFree(nm);
                                        }
                                    }
                                    Out(L"\n       我们的列: %ls\n", present ? L"可见" : L"未显示");

                                    bool forceAdd    = action && wcscmp(action, L"force-add") == 0;
                                    bool forceRemove = action && wcscmp(action, L"force-remove") == 0;

                                    if ((forceAdd || (action && wcscmp(action, L"add") == 0 && hasRvt))
                                        && !present)
                                    {
                                        std::vector<PROPERTYKEY> nk(keys);
                                        nk.push_back(PKEY_RvtProductYear);
                                        HRESULT h2 = cm->SetColumns(nk.data(), (UINT)nk.size());
                                        Out(L"       -> SetColumns(add) hr=0x%08lX\n", (unsigned long)h2);
                                        if (SUCCEEDED(h2)) changed++;
                                    }
                                    else if ((forceRemove || (action && wcscmp(action, L"remove") == 0 && !hasRvt))
                                             && present)
                                    {
                                        std::vector<PROPERTYKEY> nk;
                                        for (UINT k = 0; k < n; k++)
                                            if (!IsEqualGUID(keys[k].fmtid, FMTID_RvtVersion))
                                                nk.push_back(keys[k]);
                                        HRESULT h2 = cm->SetColumns(nk.data(), (UINT)nk.size());
                                        Out(L"       -> SetColumns(remove) hr=0x%08lX\n", (unsigned long)h2);
                                        if (SUCCEEDED(h2)) changed++;
                                    }
                                }
                            }
                            cm->Release();
                        }
                        fv->Release();
                    }
                    sv->Release();
                }
            }
            sb->Release();
        }
        disp->Release();
    }

    Out(L"\n  含 Revit 文件的窗口: %d\n", rvtFolders);
    if (action) Out(L"  已改动: %d\n", changed);
    sw->Release();
    return 0;
}

// --close-windows <子串>
// 关闭标题匹配的资源管理器窗口。用途：验证 SetColumns 是否真的落盘 ——
// 必须让窗口正常关闭（走 WM_CLOSE 让 Shell 保存视图状态），
// 再用新进程重新打开该文件夹看列还在不在。
// 这里用 IWebBrowser2::Quit，比 PostMessage 更接近用户点 X 的路径。
static int CloseWindows(const wchar_t* needle)
{
    Out(L"\n=== 关闭匹配窗口: %ls ===\n", needle ? needle : L"(全部)");

    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_IShellWindows, (void**)&sw)) || !sw)
    {
        Out(L"[x] 拿不到 IShellWindows\n");
        return 1;
    }

    long count = 0;
    sw->get_Count(&count);

    int closed = 0;
    // 反向遍历：Quit 会让集合重排
    for (long i = count - 1; i >= 0; i--)
    {
        VARIANT vi;
        VariantInit(&vi);
        vi.vt = VT_I4;
        vi.lVal = i;

        IDispatch* disp = nullptr;
        if (FAILED(sw->Item(vi, &disp)) || !disp) { VariantClear(&vi); continue; }
        VariantClear(&vi);

        IServiceProvider* sp = nullptr;
        IShellBrowser* sb = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_IServiceProvider, (void**)&sp)) && sp)
        {
            sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, (void**)&sb);
            sp->Release();
        }

        if (sb)
        {
            std::wstring folder = WindowFolderPath(sb);
            bool match = !needle || folder.find(needle) != std::wstring::npos;

            if (match)
            {
                IWebBrowser2* wb = nullptr;
                if (SUCCEEDED(disp->QueryInterface(IID_IWebBrowser2, (void**)&wb)) && wb)
                {
                    Out(L"  关闭: %ls\n", folder.empty() ? L"<非文件系统>" : folder.c_str());
                    wb->Quit();
                    wb->Release();
                    closed++;
                }
            }
            sb->Release();
        }
        disp->Release();
    }

    Out(L"  已关闭 %d 个窗口\n", closed);
    sw->Release();
    return closed > 0 ? 0 : 2;
}

// --bag-hashes
// 打印每个 Bag 下 ColInfo 的哈希。用途：在 SetColumns 前后各跑一次，
// 对比哪个 Bag 变了 —— 这样不需要解析 BagMRU 的 SHITEMID 就能定位
// 「列设置到底写进了哪个 Bag」，是验证持久化的硬证据。
static int DumpBagHashes()
{
    const std::wstring bags =
        L"SOFTWARE\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\Bags";

    std::vector<std::wstring> slots;
    if (!SubKeyNames(HKEY_CURRENT_USER, bags, slots))
    {
        Out(L"[x] 打不开 Bags\n");
        return 1;
    }

    int shown = 0, withOurs = 0;
    for (const auto& slot : slots)
    {
        // 真实层级：Bags\<slot>\{Shell,ShellNoRoam}\<FolderTypeGUID>\[ColInfo]
        static const wchar_t* kinds[] = { L"Shell", L"ShellNoRoam" };
        for (const auto* kind : kinds)
        {
            const std::wstring kindPath = bags + L"\\" + slot + L"\\" + kind;
            std::vector<std::wstring> views;
            if (!SubKeyNames(HKEY_CURRENT_USER, kindPath, views)) continue;

            for (const auto& v : views)
            {
                const std::wstring path = kindPath + L"\\" + v;

                HKEY k = nullptr;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
                    continue;

                DWORD cb = 0;
                LONG q = RegQueryValueExW(k, L"ColInfo", nullptr, nullptr, nullptr, &cb);
                if (q != ERROR_SUCCESS || cb == 0) { RegCloseKey(k); continue; }

                std::vector<BYTE> data(cb);
                if (RegQueryValueExW(k, L"ColInfo", nullptr, nullptr, data.data(), &cb) != ERROR_SUCCESS)
                { RegCloseKey(k); continue; }
                RegCloseKey(k);

                // FNV-1a，够用来判等
                unsigned long long h = 1469598103934665603ULL;
                for (BYTE b : data) { h ^= b; h *= 1099511628211ULL; }

                // 顺带看看里面有没有我们的 FMTID（AE96A02E-F8A8-4063-80CD-E90FDF050B70）
                static const BYTE sig[] = { 0x2E, 0xA0, 0x96, 0xAE, 0xA8, 0xF8, 0x63, 0x40,
                                            0x80, 0xCD, 0xE9, 0x0F, 0xDF, 0x05, 0x0B, 0x70 };
                bool hasOurs = false;
                if (data.size() >= sizeof(sig))
                    for (size_t i = 0; i + sizeof(sig) <= data.size(); i++)
                        if (memcmp(&data[i], sig, sizeof(sig)) == 0) { hasOurs = true; break; }

                if (hasOurs) withOurs++;
                Out(L"  %-6ls %-11ls %-40ls cb=%-4lu hash=%016llX %ls\n",
                    slot.c_str(), kind, v.c_str(), (unsigned long)cb, h,
                    hasOurs ? L"<< 含 RvtVersion" : L"");
                shown++;
            }
        }
    }

    Out(L"\n  共 %d 个带 ColInfo 的视图，其中 %d 个含 RvtVersion\n", shown, withOurs);
    return 0;
}

// ---------------------------------------------------------------- main
int wmain(int argc, wchar_t** argv)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetConsoleOutputCP(CP_UTF8);

    int rc = 1;

    if (argc < 2)
    {
        Out(L"用法:\n");
        Out(L"  rvtvdiag --check\n");
        Out(L"  rvtvdiag --store <file.rvt>\n");
        Out(L"  rvtvdiag --dump-cols | --list-foldertypes\n");
        Out(L"  rvtvdiag --add-col <{GUID}> | --del-col <{GUID}>\n");
        CoUninitialize();
        return 1;
    }

    std::wstring mode = argv[1];

    if (mode == L"--check")            rc = Check();
    else if (mode == L"--store" && argc >= 3) rc = StoreTest(argv[2]);
    else if (mode == L"--dump-cols")   rc = DumpCols(false);
    else if (mode == L"--list-foldertypes") rc = DumpCols(true);
    else if (mode == L"--add-col" && argc >= 3) rc = EditCol(argv[2], true);
    else if (mode == L"--access" && argc >= 3) rc = AccessCheck(argv[2]);
    else if (mode == L"--del-col" && argc >= 3) rc = EditCol(argv[2], false);
    else if (mode == L"--tree" && argc >= 3) rc = TreeOf(argv[2]);
    else if (mode == L"--folder-bindings") rc = FindFolderTypeBindings();
    else if (mode == L"--user-bags") rc = DumpUserBags();
    else if (mode == L"--probe-user-paths") rc = ProbeUserPaths();
    else if (mode == L"--add-col-user" && argc >= 3) rc = EditColUser(argv[2], true);
    else if (mode == L"--del-col-user" && argc >= 3) rc = EditColUser(argv[2], false);
    else if (mode == L"--rvt-bags") rc = ListRvtBags();
    else if (mode == L"--bagmru-raw") rc = DumpBagMruRaw(argc >= 3 ? argv[2] : L"");
    else if (mode == L"--view-cols" && argc >= 3) rc = ProbeViewColumns(argc, argv);
    else if (mode == L"--set-cols" && argc >= 4)
        rc = SetFolderColumns(argv[2], std::wstring(argv[3]) == L"add");
    else if (mode == L"--real-windows")
        rc = RealWindows(argc >= 3 ? argv[2] : nullptr);
    else if (mode == L"--close-windows")
        rc = CloseWindows(argc >= 3 ? argv[2] : nullptr);
    else if (mode == L"--bag-hashes") rc = DumpBagHashes();
    else Out(L"[x] 参数不对\n");

    CoUninitialize();
    return rc;
}
