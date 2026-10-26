// =====================================================================
// RvtVersion 安装程序
//
// 单文件 exe：DLL / propdesc / schema 注册工具全部作为 RCDATA 资源嵌入，
// 运行时释放到 %ProgramFiles%\RvtVersion，然后完成全部注册。
//
// 安装后资源管理器「详细信息」视图的列里可直接看到 Revit 版本。
//
// 命令行：
//   无参数            正常安装（非管理员会自动请求提权）
//   /uninstall        卸载
//   /quiet  /S        静默（不等待按键）
//   /extract <dir>    只释放载荷到指定目录，不做任何注册（自检用）
//
// 为什么 manifest 用 asInvoker + 运行时提权，而不是 requireAdministrator：
//   这样同一个二进制在非管理员环境下也能跑 /extract，便于验证载荷完整性；
//   正常安装时检测到非管理员再用 runas 重启自己，用户体验一致。
// =====================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>      // ShellExecuteExW / ShellExecuteW
#include <sddl.h>          // ConvertStringSecurityDescriptorToSecurityDescriptorW
#include <aclapi.h>
#include <tlhelp32.h>
#include <propsys.h>
#include <propvarutil.h>
#include <conio.h>

#include <cstdio>
#include <cstdarg>
#include <clocale>     // setlocale：修窄 printf 的 %ls 截断问题
#include <string>
#include <vector>

#include "PropKeys.h"

// ---------------------------------------------------------------- 常量
static const wchar_t* kClsid   = L"{5AD3E9FC-A720-47F6-8DFA-9AACD3286E01}";
static const wchar_t* kProgId  = L"RvtVersion.PropertyHandler";
static const wchar_t* kShellEx = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";

static const wchar_t* kExts[] = { L".rvt", L".rfa", L".rte", L".rft" };
static const size_t   kExtCount = _countof(kExts);

static const wchar_t* kDllName    = L"RvtVersionHandler.dll";
static const wchar_t* kPropDesc   = L"RvtVersion.propdesc";
static const wchar_t* kSchemaTool = L"rvtvpropschema.exe";
static const wchar_t* kDiagTool   = L"rvtvdiag.exe";
static const wchar_t* kWatcher    = L"rvtvwatch.exe";
static const wchar_t* kUninstExe  = L"RvtVersionSetup.exe";

// 资源 ID，必须与 Setup.rc 一致
#define RES_DLL        101
#define RES_PROPDESC   102
#define RES_SCHEMATOOL 103
#define RES_DIAGTOOL   104
#define RES_WATCHER    105

static const wchar_t* kAppName = L"RVT-V Revit Version Column";
static const wchar_t* kVersion = L"1.2.1";

static bool g_quiet = false;
static FILE* g_log = nullptr;

// ---------------------------------------------------------------- 工具
// 同时输出到控制台与日志文件。
// 提权重启后子进程的控制台窗口是独立的，父进程拿不到输出，
// 所以必须落盘才能排查安装失败。
static void P(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);

    if (g_log)
    {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

static std::wstring ProgramFilesDir()
{
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &p)) && p)
    {
        out = p;
        CoTaskMemFree(p);
    }
    if (out.empty())
    {
        wchar_t buf[MAX_PATH] = { 0 };
        GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH);
        out = buf;
    }
    return out;
}

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

static bool IsAdmin()
{
    BOOL admin = FALSE;
    PSID g = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &g))
    {
        CheckTokenMembership(nullptr, g, &admin);
        FreeSid(g);
    }
    return admin != FALSE;
}

static void WaitKey()
{
    if (g_quiet) return;
    P("\n按任意键关闭...");
    _getch();
    P("\n");
}

// ---------------------------------------------------------------- 注册表
// 注意：默认值的 value name 必须是 nullptr，这是 Win32 的定义。
// 这里和 PowerShell 的 Set-ItemProperty -Name '(default)' 那个坑是同一个问题。
static bool RegSetStr(HKEY root, const std::wstring& sub, const wchar_t* name,
                      const std::wstring& val, REGSAM extra = 0)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE | extra,
                        nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG rc = RegSetValueExW(k, name, 0, REG_SZ,
                             (const BYTE*)val.c_str(),
                             (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

static bool RegSetDword(HKEY root, const std::wstring& sub, const wchar_t* name, DWORD val)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG rc = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

static bool RegGetStr(HKEY root, const std::wstring& sub, const wchar_t* name, std::wstring& out,
                      REGSAM extra = 0)
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
    buf[cb / sizeof(wchar_t)] = 0;
    out = buf;
    return true;
}

static bool KeyExists(HKEY root, const std::wstring& sub)
{
    HKEY k = nullptr;
    bool ok = RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS;
    if (ok) RegCloseKey(k);
    return ok;
}

// ---------------------------------------------------------------- 载荷
static bool ExtractRes(int id, const std::wstring& outPath)
{
    HRSRC hr = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!hr) return false;

    HGLOBAL hg = LoadResource(nullptr, hr);
    if (!hg) return false;

    DWORD sz = SizeofResource(nullptr, hr);
    const void* p = LockResource(hg);
    if (!p || sz == 0) return false;

    HANDLE f = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(f, p, sz, &written, nullptr);
    CloseHandle(f);
    return ok && written == sz;
}

// ---------------------------------------------------------------- 载荷清单
// 唯一的载荷定义处。加/减安装文件时只改这张表 + Setup.rc 里对应的 RCDATA，
// kPayloadCount 自动跟随，不会再出现「表是 5、校验写死 4」这类自相矛盾。
// （1.2.1 之前就踩过：加了 rvtvwatch.exe 后 kPayloadCount 改成 5，
//   但 DoInstall 里的 if (n != 4) 忘了改，导致每次安装都在 [3/7] 自我中止。）
struct Payload { int id; const wchar_t* name; };

static const Payload kPayloads[] = {
    { RES_DLL,        kDllName    },
    { RES_PROPDESC,   kPropDesc   },
    { RES_SCHEMATOOL, kSchemaTool },
    { RES_DIAGTOOL,   kDiagTool   },
    { RES_WATCHER,    kWatcher    },
};

static const int kPayloadCount = (int)_countof(kPayloads);

// 释放全部载荷；返回成功释放的数量
static int ExtractAll(const std::wstring& dir)
{
    int ok = 0;
    for (size_t i = 0; i < _countof(kPayloads); i++)
    {
        std::wstring dst = dir + L"\\" + kPayloads[i].name;
        if (ExtractRes(kPayloads[i].id, dst))
        {
            WIN32_FILE_ATTRIBUTE_DATA fad = { 0 };
            GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &fad);
            ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            P("      %-26ls %10llu bytes\n", kPayloads[i].name, sz);
            ok++;
        }
        else
        {
            P("      [!] 释放失败: %ls\n", kPayloads[i].name);
        }
    }
    return ok;
}

// ---------------------------------------------------------------- 进程
static void KillExplorer()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe = { 0 };
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0)
            {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h)
                {
                    TerminateProcess(h, 0);
                    CloseHandle(h);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static bool ExplorerRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    PROCESSENTRY32W pe = { 0 };
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// 重启资源管理器让新注册的属性处理器与列生效。
// 先靠系统的 AutoRestartShell 自动拉起（默认开启），等不到再手动启动。
static void RestartExplorer()
{
    KillExplorer();
    Sleep(1800);

    if (!ExplorerRunning())
    {
        ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
        Sleep(1200);
    }

    if (ExplorerRunning()) P("  [+] 资源管理器已重启\n");
    else                   P("  [!] 资源管理器未自动重启，请手动打开任务管理器启动 explorer.exe\n");
}

// ---------------------------------------------------------------- 校验
static int VerifySchema()
{
    IPropertySystem* sys = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_PropertySystem, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IPropertySystem, (void**)&sys);
    if (FAILED(hr) || !sys) return -1;

    int found = 0;
    for (size_t i = 0; i < kPropCount; i++)
    {
        IPropertyDescription* desc = nullptr;
        HRESULT h = sys->GetPropertyDescription(kProps[i].key, IID_IPropertyDescription, (void**)&desc);
        if (SUCCEEDED(h) && desc)
        {
            found++;
            desc->Release();
        }
        else if (i == 0)
        {
            // 只打第一个，避免刷屏；这一条能反映整体问题
            P("      propID %lu 查询失败 hr=0x%08lX\n",
              kProps[i].key.pid, (unsigned long)h);
        }
    }
    sys->Release();
    return found;
}

static DWORD DirSizeKB(const std::wstring& dir)
{
    ULONGLONG total = 0;
    WIN32_FIND_DATAW fd = { 0 };
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            total += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return (DWORD)(total / 1024);
}

// ---------------------------------------------------------------- 提权
static bool RelaunchElevated(const std::wstring& args)
{
    wchar_t self[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    // 关键：先把父进程的日志句柄关掉再拉起子进程。
    // 否则子进程用 fopen(..., "a") 打开同一个日志会因共享冲突失败，
    // 提权后的全部输出都丢失——这正是之前「只看得到 4 行」的原因。
    if (g_log)
    {
        P("[*] 即将提权，移交日志给子进程\n");
        fclose(g_log);
        g_log = nullptr;
    }

    SHELLEXECUTEINFOW sei = { 0 };
    sei.cbSize       = sizeof(sei);
    sei.lpVerb       = L"runas";
    sei.lpFile       = self;
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.nShow        = SW_SHOWNORMAL;
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;

    if (!ShellExecuteExW(&sei))
        return false;

    if (sei.hProcess)
    {
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD rc = 1;
        GetExitCodeProcess(sei.hProcess, &rc);
        CloseHandle(sei.hProcess);
        return rc == 0;
    }
    return true;
}

// 把注册表里所有指向 RvtVersion.propdesc 的旧 schema 条目清掉。
// 原因：PSRegisterPropertySchema 对「已注册但内容需要更新」的情况不会自动生效，
// 它会认为同一个文件已经注册过而直接返回 ERROR_ALREADY_EXISTS，
// 属性系统继续用旧的（错误的）解析结果。必须先彻底注销。
static void PurgeOldSchemas(const std::wstring& pdPath)
{
    const wchar_t* base = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertySchema";

    HKEY baseKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, base, 0, KEY_READ, &baseKey) != ERROR_SUCCESS)
        return;

    std::vector<std::wstring> toDelete;
    DWORD i = 0;
    wchar_t name[256];
    DWORD cch = 0;
    while (true)
    {
        cch = _countof(name);
        if (RegEnumKeyExW(baseKey, i++, name, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        std::wstring sub = std::wstring(base) + L"\\" + name;
        std::wstring val;
        if (RegGetStr(HKEY_LOCAL_MACHINE, sub, nullptr, val))
        {
            if (_wcsicmp(val.c_str(), pdPath.c_str()) == 0)
                toDelete.push_back(name);
        }
    }
    RegCloseKey(baseKey);

    for (size_t k = 0; k < toDelete.size(); k++)
    {
        std::wstring sub = std::wstring(base) + L"\\" + toDelete[k];
        // 先尝试走官方注销（能清掉 URI / CompactURI 缓存）
        std::wstring val;
        if (RegGetStr(HKEY_LOCAL_MACHINE, sub, nullptr, val))
            PSUnregisterPropertySchema(val.c_str());
        SHDeleteKeyW(HKEY_LOCAL_MACHINE, sub.c_str());
        P("      清理旧 schema 条目 %ls\n", toDelete[k].c_str());
    }
}

// =====================================================================
// 默认列布局：让「Revit Version」装完就出现在列里
//
// 结构（实测本机 Win10 22H2）：
//   HKLM\...\Explorer\FolderTypes\{<FolderTypeGUID>}
//       \TopViews\{00000000-0000-0000-0000-000000000000}
//           [ColumnList] = REG_SZ
//               "prop:0(34)System.ItemNameDisplay;0System.DateModified;..."
//           [SortByList] = "prop:System.ItemNameDisplay"
//
// ColumnList 语法（实测本机 Win10 22H2 的真实值）：
//   "prop:0(34)System.ItemNameDisplay;0System.DateModified;0System.ItemTypeText;..."
//   分号分隔的列项；每项 "可见性(宽度)canonical-name"
//     可见性 0 = 显示，1 = 隐藏
//     括号里是列宽，可省略
//   "prop:" 前缀只在整串开头出现一次，后续列项不能再带。
//
// 只改「通用文件夹」一个 FolderType 会污染所有文件夹，所以这里同时提供
// 针对具体 FolderType 的操作，由安装器决定改哪些。
// =====================================================================
static bool EditColumnList(const std::wstring& folderTypeGuid,
                           const std::wstring& canonicalName,
                           bool add, std::wstring* resulting = nullptr)
{
    const std::wstring sub =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FolderTypes\\" +
        folderTypeGuid + L"\\TopViews\\{00000000-0000-0000-0000-000000000000}";

    std::wstring cur;
    if (!RegGetStr(HKEY_LOCAL_MACHINE, sub, L"ColumnList", cur, KEY_WOW64_64KEY))
        return false;

    // 去掉开头的 "prop:"，统一按「无前缀」处理，最后写回时再补上
    if (cur.compare(0, 5, L"prop:") == 0)
        cur = cur.substr(5);

    // 拆成列项
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

    // 去掉已存在的同名项（不管 add 还是 del）
    std::vector<std::wstring> kept;
    for (const auto& it : items)
    {
        if (it.find(canonicalName) == std::wstring::npos)
            kept.push_back(it);
    }

    if (add)
    {
        // 可见性 0 = 默认显示；宽度 14 与 propdesc 里的 defaultColumnWidth 一致
        kept.push_back(L"0(14)" + canonicalName);
    }

    std::wstring out = L"prop:";
    for (size_t i = 0; i < kept.size(); i++)
    {
        if (i) out += L";";
        out += kept[i];
    }

    // 这里刻意不用 RegSetStr：
    // 该键已存在且受系统保护，RegCreateKeyExW 带 KEY_WRITE 会失败。
    // 直接 Open + KEY_SET_VALUE 才是可靠路径。
    HKEY k = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0,
                            KEY_SET_VALUE | KEY_WOW64_64KEY, &k);
    if (rc != ERROR_SUCCESS)
    {
        P("      [x] 打开键失败 rc=%ld\n", rc);
        return false;
    }

    rc = RegSetValueExW(k, L"ColumnList", 0, REG_SZ, (const BYTE*)out.c_str(),
                        (DWORD)((out.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);

    if (rc != ERROR_SUCCESS)
    {
        P("      [x] 写 ColumnList 失败 rc=%ld\n", rc);
        return false;
    }

    if (resulting) *resulting = out;
    return true;
}

// 单独设置 / 撤销默认列布局，便于在不重装的情况下重试。
// 安装流程中提权子进程的日志与父进程共用同一文件，
// 出问题时看不到子进程输出，所以这里做成可独立执行的命令。
static int DoFixColumns(bool add)
{
    // P() 是窄字符输出（%s 期望 char*），这里不能传宽字符串，
    // 否则 vprintf 会把 wchar_t* 当地址解读，输出乱码。
    P("\n=== %s默认列布局 ===\n\n", add ? "写入" : "撤销");

    struct FtTarget { const wchar_t* guid; const wchar_t* label; };
    const FtTarget ftTargets[] = {
        { L"{5c4f28b5-f869-4e84-8e60-f11db97c5cc7}", L"Generic  (通用文件夹)" },
        { L"{7d49d726-3c21-4f05-99aa-fdc2c9474656}", L"Documents(文档)" },
    };

    int okCount = 0;
    for (size_t i = 0; i < _countof(ftTargets); i++)
    {
        const std::wstring sub =
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FolderTypes\\" +
            std::wstring(ftTargets[i].guid) +
            L"\\TopViews\\{00000000-0000-0000-0000-000000000000}";

        std::wstring before;
        bool readable = RegGetStr(HKEY_LOCAL_MACHINE, sub, L"ColumnList", before,
                                  KEY_WOW64_64KEY);
        P("[%zu] %ls\n", i + 1, ftTargets[i].label);
        P("    键路径 : %ls\n", sub.c_str());
        P("    可读   : %ls\n", readable ? L"YES" : L"NO");
        if (readable)
            P("    原值   : %ls\n", before.c_str());

        std::wstring result;
        bool ok = EditColumnList(ftTargets[i].guid, L"RvtVersion.ProductYear", add, &result);
        P("    写入   : %ls\n", ok ? L"OK" : L"FAIL");
        if (ok)
        {
            P("    新值   : %ls\n", result.c_str());
            // 回读确认
            std::wstring back;
            if (RegGetStr(HKEY_LOCAL_MACHINE, sub, L"ColumnList", back, KEY_WOW64_64KEY))
            {
                bool has = back.find(L"RvtVersion.ProductYear") != std::wstring::npos;
                P("    回读   : %ls（含我们的列: %ls）\n",
                  back.c_str(), has ? L"是" : L"否");
                if (has == add) okCount++;
            }
        }
        P("\n");
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    P("结果: %d / %zu 成功\n", okCount, _countof(ftTargets));
    return okCount == (int)_countof(ftTargets) ? 0 : 1;
}

// =====================================================================
// 安装
// =====================================================================
static int DoInstall()
{
    const std::wstring target = ProgramFilesDir() + L"\\RvtVersion";
    const std::wstring dllPath = target + L"\\" + kDllName;
    const std::wstring pdPath  = target + L"\\" + kPropDesc;

    P("\n=== RVT-V 安装：资源管理器 Revit 版本列 ===\n\n");
    P("  安装目录: %ls\n\n", target.c_str());

    // ---- 1. 停掉资源管理器，释放 DLL 占用 ----
    P("[1/7] 停止资源管理器以释放文件占用\n");
    KillExplorer();
    Sleep(1200);
    P("      已停止\n");

    // ---- 2. 建目录 ----
    P("[2/7] 创建安装目录\n");
    CreateDirectoryW(target.c_str(), nullptr);
    if (!PathFileExistsW(target.c_str()))
    {
        P("  [x] 无法创建 %ls\n", target.c_str());
        RestartExplorer();
        return 1;
    }
    P("      就绪\n");

    // ---- 3. 释放载荷 ----
    P("[3/7] 释放文件\n");
    int n = ExtractAll(target);
    // 必须用 kPayloadCount 而不是字面量：载荷数量随版本变化
    // （1.2.0 起从 4 个增加到 5 个，新增 rvtvwatch.exe），
    // 写死数字会让新版安装器在第一步就自我中止。
    if (n != kPayloadCount)
    {
        P("  [x] 载荷释放不完整（%d/%d），安装中止\n", n, kPayloadCount);
        RestartExplorer();
        return 1;
    }

    // 把安装器自己也放一份，供卸载使用
    wchar_t self[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    CopyFileW(self, (target + L"\\" + kUninstExe).c_str(), FALSE);

    // 清掉上一代 overlay 方案的残留（注册项 + 文件）。
    // 旧方案是 ShellIconOverlayIdentifiers + .NET DLL，和本工具的属性处理器
    // 互不相干；留着会让图标角标和列同时出现，而且 DLL 被资源管理器占用删不掉。
    {
        // 1) 移除 overlay 注册项
        const wchar_t* ovBase =
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers";

        HKEY ovKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ovBase, 0, KEY_READ, &ovKey) == ERROR_SUCCESS)
        {
            std::vector<std::wstring> victims;
            wchar_t name[256];
            DWORD i = 0, cch = 0;
            while (true)
            {
                cch = _countof(name);
                if (RegEnumKeyExW(ovKey, i++, name, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                    break;
                // 键名形如 "  RvtVersion"（前面补空格抢槽位）
                if (wcsstr(name, L"RvtVersion")) victims.push_back(name);
            }
            RegCloseKey(ovKey);

            for (size_t k = 0; k < victims.size(); k++)
            {
                std::wstring full = std::wstring(ovBase) + L"\\" + victims[k];
                SHDeleteKeyW(HKEY_LOCAL_MACHINE, full.c_str());
                P("      清理旧版图标覆盖层: %ls\n", victims[k].c_str());
            }
        }

        // 2) 移除旧 CLSID
        const wchar_t* oldClsid = L"SOFTWARE\\Classes\\CLSID\\{B7E2D94A-3C61-4F8B-A5D2-9E4C1A7F6B83}";
        if (KeyExists(HKEY_LOCAL_MACHINE, oldClsid))
        {
            SHDeleteKeyW(HKEY_LOCAL_MACHINE, oldClsid);
            P("      清理旧版 CLSID\n");
        }
        const wchar_t* oldProgId = L"SOFTWARE\\Classes\\RvtVersion.VersionOverlay";
        if (KeyExists(HKEY_LOCAL_MACHINE, oldProgId))
        {
            SHDeleteKeyW(HKEY_LOCAL_MACHINE, oldProgId);
            P("      清理旧版 ProgID\n");
        }

        // 3) 删残留 DLL（此时 explorer 已停，可以删掉）
        const wchar_t* legacy[] = {
            L"RevitVersion.Core.dll",
            L"RevitVersionShell.dll",
        };
        for (size_t i = 0; i < _countof(legacy); i++)
        {
            std::wstring f = target + L"\\" + legacy[i];
            if (PathFileExistsW(f.c_str()))
            {
                if (DeleteFileW(f.c_str()))
                    P("      清理旧版文件: %ls\n", legacy[i]);
                else
                    MoveFileExW(f.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
        }

        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }

    // 所有用户可读（资源管理器以普通用户身份加载 DLL）
    {
        wchar_t sddl[] = L"D:PAI(A;OICI;GRGX;;;BU)(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)";
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, SDDL_REVISION_1, &sd, nullptr))
        {
            SECURITY_INFORMATION si = DACL_SECURITY_INFORMATION;
            SetFileSecurityW(target.c_str(), si, sd);
            LocalFree(sd);
        }
    }

    // ---- 4. COM 类 ----
    P("[4/7] 注册 COM 类\n");
    {
        std::wstring clsidKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsid;
        RegSetStr(HKEY_LOCAL_MACHINE, clsidKey, nullptr, L"RvtVersion Revit Version Property Handler");
        RegSetStr(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", nullptr, dllPath);
        RegSetStr(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Both");
        RegSetStr(HKEY_LOCAL_MACHINE, clsidKey + L"\\ProgID", nullptr, kProgId);

        std::wstring progKey = std::wstring(L"SOFTWARE\\Classes\\") + kProgId;
        RegSetStr(HKEY_LOCAL_MACHINE, progKey, nullptr, L"RvtVersion Revit Version Property Handler");
        RegSetStr(HKEY_LOCAL_MACHINE, progKey + L"\\CLSID", nullptr, kClsid);

        std::wstring check;
        if (RegGetStr(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", nullptr, check) &&
            _wcsicmp(check.c_str(), dllPath.c_str()) == 0)
        {
            P("      InprocServer32 -> %s\n", Narrow(dllPath).c_str());
        }
        else
        {
            P("  [x] InprocServer32 写入校验失败\n");
            RestartExplorer();
            return 1;
        }
    }

    // ---- 5. 属性描述 schema ----
    P("[5/7] 属性描述 schema\n");
    {
        // 先确认 propdesc 真的在磁盘上、且可读
        WIN32_FILE_ATTRIBUTE_DATA fad = { 0 };
        if (!GetFileAttributesExW(pdPath.c_str(), GetFileExInfoStandard, &fad))
        {
            P("  [x] propdesc 不存在: %s\n", Narrow(pdPath).c_str());
            P("      GetLastError=%lu\n", GetLastError());
            RestartExplorer();
            return 1;
        }
        P("      文件: %s (%lu bytes)\n", Narrow(pdPath).c_str(), fad.nFileSizeLow);

        // 彻底清掉旧注册，否则 PSRegisterPropertySchema 认为「已注册」不更新解析结果
        PurgeOldSchemas(pdPath);

        HRESULT hr = PSRegisterPropertySchema(pdPath.c_str());
        P("      PSRegisterPropertySchema -> hr=0x%08lX\n", (unsigned long)hr);

        if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        {
            P("      仍报已存在，注销后再试\n");
            HRESULT hu = PSUnregisterPropertySchema(pdPath.c_str());
            P("      PSUnregisterPropertySchema -> hr=0x%08lX\n", (unsigned long)hu);
            hr = PSRegisterPropertySchema(pdPath.c_str());
            P("      重新注册 -> hr=0x%08lX\n", (unsigned long)hr);
        }

        if (FAILED(hr))
        {
            P("  [x] schema 注册失败\n");
            if (hr == E_ACCESSDENIED) P("      原因: 权限不足\n");
            else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) P("      原因: 文件找不到\n");
            else if (hr == HRESULT_FROM_WIN32(ERROR_BAD_FORMAT)) P("      原因: XML 格式不合法\n");
            else P("      原因: 未知，hr=0x%08lX\n", (unsigned long)hr);
            P("      排查: 检查 %s 是否为合法 XML\n", Narrow(pdPath).c_str());
            RestartExplorer();
            return 1;
        }

        PSRefreshPropertySchema();
        Sleep(300);

        int got = VerifySchema();
        P("      属性注册: %d / %zu\n", got, kPropCount);
        if (got != (int)kPropCount)
        {
            P("  [x] 属性数量不符，安装中止\n");
            P("      可能原因: propdesc 里的 formatID 与 DLL 里的 FMTID 不一致\n");
            RestartExplorer();
            return 1;
        }
    }

    // ---- 6. 挂到文件类型 ----
    P("[6/7] 挂载到 .rvt / .rfa / .rte / .rft\n");
    {
        const std::wstring propList =
            L"prop:System.ItemNameDisplay;RvtVersion.ProductYear;RvtVersion.Build;System.Size";

        const std::wstring propHandlersRoot =
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers";

        for (size_t i = 0; i < kExtCount; i++)
        {
            std::wstring ext = kExts[i];

            // (a) ShellEx 属性处理器
            //     兼容旧路径，部分组件仍按这里查。
            std::wstring seKey = L"SOFTWARE\\Classes\\" + ext + L"\\ShellEx\\" + kShellEx;
            RegSetStr(HKEY_LOCAL_MACHINE, seKey, nullptr, kClsid);

            // (b) PropertyHandlers —— Win7+ 资源管理器取 IPropertyStore 的
            //     集中式主查找路径。缺这一项的症状就是：列头能出现（schema 已注册），
            //     但每一行都是空白（handler 根本没被调用）。
            //     必须用 64 位视图写，32 位写进去 64 位 Shell 看不到。
            std::wstring phKey = propHandlersRoot + L"\\" + ext;
            HKEY ph = nullptr;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, phKey.c_str(), 0, nullptr, 0,
                                KEY_WRITE | KEY_WOW64_64KEY, nullptr, &ph, nullptr)
                == ERROR_SUCCESS)
            {
                RegSetValueExW(ph, nullptr, 0, REG_SZ, (const BYTE*)kClsid,
                               (DWORD)((wcslen(kClsid) + 1) * sizeof(wchar_t)));
                RegCloseKey(ph);
            }

            // (c) 预览窗格 + 悬停提示
            //     位置在 SystemFileAssociations，不在 ShellEx 下。
            std::wstring sfa = L"SOFTWARE\\Classes\\SystemFileAssociations\\" + ext;
            RegSetStr(HKEY_LOCAL_MACHINE, sfa, L"PreviewDetails", propList);
            RegSetStr(HKEY_LOCAL_MACHINE, sfa, L"InfoTip", propList);

            // 双路校验，两条都通过才算成功
            std::wstring gotSe, gotPh;
            bool okSe = RegGetStr(HKEY_LOCAL_MACHINE, seKey, nullptr, gotSe) &&
                        _wcsicmp(gotSe.c_str(), kClsid) == 0;
            bool okPh = RegGetStr(HKEY_LOCAL_MACHINE, phKey, nullptr, gotPh,
                                  KEY_WOW64_64KEY) &&
                        _wcsicmp(gotPh.c_str(), kClsid) == 0;

            P("      %-6ls ShellEx=%-4ls PropHandlers=%-4ls\n",
              ext.c_str(), okSe ? L"OK" : L"FAIL", okPh ? L"OK" : L"FAIL");

            if (!okSe || !okPh)
            {
                P("  [x] %ls 注册未生效，安装中止\n", ext.c_str());
                RestartExplorer();
                return 1;
            }
        }
    }

    // ---- 7. 卸载信息 + 默认列布局 + 刷新 ----
    P("[7/7] 注册卸载信息、默认列布局并刷新\n");
    {
        std::wstring uninst = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RvtVersion";
        std::wstring cmdLine = L"\"" + target + L"\\" + kUninstExe + L"\" /uninstall";

        RegSetStr(HKEY_LOCAL_MACHINE, uninst, L"DisplayName", L"RVT-V · 资源管理器 Revit 版本列");
        RegSetStr(HKEY_LOCAL_MACHINE, uninst, L"DisplayVersion", kVersion);
        RegSetStr(HKEY_LOCAL_MACHINE, uninst, L"Publisher", L"RVT-V");
        RegSetStr(HKEY_LOCAL_MACHINE, uninst, L"InstallLocation", target);
        RegSetStr(HKEY_LOCAL_MACHINE, uninst, L"UninstallString", cmdLine);
        RegSetStr(HKEY_LOCAL_MACHINE, uninst, L"QuietUninstallString", cmdLine + L" /quiet");
        RegSetDword(HKEY_LOCAL_MACHINE, uninst, L"NoModify", 1);
        RegSetDword(HKEY_LOCAL_MACHINE, uninst, L"NoRepair", 1);
        RegSetDword(HKEY_LOCAL_MACHINE, uninst, L"EstimatedSize", DirSizeKB(target));

        // 默认列布局：把 Revit Version 加进通用文件夹与文档文件夹的默认列。
        //
        // 注意这里只写 HKLM 的 FolderTypes（受 SYSTEM 保护，通常 rc=5 写不进）。
        // 真正的条件化显示由 rvtvwatch.exe 负责，不依赖这一步 —— 保留这段是为了
        // 万一能写成功（某些系统 ACL 较松），让新文件夹天生就带这一列。
        struct FtTarget { const wchar_t* guid; const wchar_t* label; };
        const FtTarget ftTargets[] = {
            { L"{5c4f28b5-f869-4e84-8e60-f11db97c5cc7}", L"Generic  (通用文件夹)" },
            { L"{7d49d726-3c21-4f05-99aa-fdc2c9474656}", L"Documents(文档)" },
        };

        int colOk = 0;
        for (size_t i = 0; i < _countof(ftTargets); i++)
        {
            std::wstring result;
            bool ok = EditColumnList(ftTargets[i].guid, L"RvtVersion.ProductYear",
                                     true, &result);
            if (ok) colOk++;
            P("      默认列 %-22ls %ls\n", ftTargets[i].label,
              ok ? L"已勾选" : L"系统保护，交给监视器处理");
        }

        if (colOk > 0)
            P("      %d / %zu 个类型已写入默认列\n", colOk, _countof(ftTargets));
        else
            P("      系统默认列由 SYSTEM 保护，本次不改（不影响使用）\n");

        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        P("      完成\n");
    }

    // ---- 常驻监视器 ----
    // 这是「含 Revit 文件才显示该列」的实际执行者。
    // 装完就注册开机启动并立即拉起，用户不用做任何事。
    P("\n[*] 启用按文件夹自动显示列\n");

    {
        std::wstring watcher = target + L"\\" + kWatcher;

        // 先杀掉可能存在的旧实例，避免两个进程同时对同一窗口 SetColumns
        {
            HANDLE h = CreateMutexW(nullptr, FALSE, L"RvtColumnWatcher_SingleInstance");
            if (h)
            {
                // 拿不到所有权说明已有实例在跑；让它自己退出太复杂，
                // 直接 taskkill 更可靠（用户看到的现象就是「装完新版本没生效」）。
                if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0)
                {
                    std::wstring kill = L"taskkill /F /IM " + std::wstring(kWatcher);
                    _wsystem(kill.c_str());
                    Sleep(300);
                }
                CloseHandle(h);
            }
        }

        // 开机启动（HKCU Run，免提权）
        HKEY run = nullptr;
        bool autostart = false;
        if (RegCreateKeyExW(HKEY_CURRENT_USER,
                            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                            0, nullptr, 0, KEY_SET_VALUE, nullptr, &run, nullptr)
            == ERROR_SUCCESS)
        {
            std::wstring cmd = L"\"" + watcher + L"\" --silent";
            LONG rc = RegSetValueExW(run, L"RvtColumnWatcher", 0, REG_SZ,
                                     (const BYTE*)cmd.c_str(),
                                     (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(run);
            autostart = (rc == ERROR_SUCCESS);
        }
        P("      开机启动 %ls\n", autostart ? L"已注册" : L"注册失败");

        // 立即拉起监视器。
        //
        // 关键点：不能用 CreateProcessW 直接从安装器起进程 ——
        // 安装器是提权子进程，它一退出，用 CreateProcessW 起的子进程会跟着
        // 被杀掉（实测：日志显示「监视器已启动 pid=44612」，但进程立刻消失）。
        // 改为经由 explorer.exe 代启：explorer 常驻，
        // 进程的父级落在 explorer 下，与安装器生命周期解耦。
        std::wstring cmdLine = L"\"" + watcher + L"\" --silent";

        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask  = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        sei.lpVerb = L"open";
        sei.lpFile = watcher.c_str();
        sei.lpParameters = L"--silent";
        sei.lpDirectory  = target.c_str();
        sei.nShow  = SW_HIDE;

        if (ShellExecuteExW(&sei))
        {
            P("      监视器已启动\n");
            Sleep(600);     // 给它一轮时间，装完立刻能看到列
        }
        else
        {
            P("      [!] ShellExecute 启动失败 err=%lu，改用 CreateProcess\n", GetLastError());

            // 兜底：即使父进程退出被带走，至少本次安装结束时是活的；
            // 用户下次登录由 Run 键自动拉起。
            STARTUPINFOW si = { sizeof(si) };
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi = {};
            std::wstring cl = cmdLine;

            if (CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                               target.c_str(), &si, &pi))
            {
                P("      监视器已启动 pid=%lu\n", pi.dwProcessId);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            else
            {
                P("      [!] 启动失败 err=%lu（下次登录仍会自动启动）\n", GetLastError());
            }
        }

        P("      规则: 文件夹含 .rvt/.rfa/.rte/.rft -> 自动显示该列\n");
        P("            不含                        -> 自动隐藏该列\n");
    }

    // ---- 重启资源管理器 ----
    P("\n[*] 重启资源管理器\n");
    RestartExplorer();

    P("\n========================================\n");
    P(" 安装完成\n");
    P("========================================\n\n");
    P("现在怎么看到版本：\n");
    P("  打开任意含 .rvt 的文件夹，切到「详细信息」视图 —— 列会自动出现。\n");
    P("  不含 Revit 文件的文件夹不会有这一列，不留空列。\n\n");
    P("不需要手动勾选，也不需要重启电脑。\n");
    P("若某个窗口没立刻生效，切走再切回该文件夹即可（或等 1 秒）。\n\n");
    P("可用的列（列头右键 -> 其他... 里还能加）：\n");
    P("  Revit Version      2023 / 12.0 / 2012   自动显示的就是这一列\n");
    P("  Revit Year         纯年份，便于排序\n");
    P("  Revit Format       数值，可筛选 >= 2021\n");
    P("  Revit Build        构建号\n");
    P("  Revit Locale       语言\n");
    P("  Revit Worksharing  Central / -\n\n");
    P("监视器控制：\n");
    P("  %s --status    看当前各窗口状态\n", Narrow(target + L"\\" + kWatcher).c_str());
    P("  %s --remove    关闭开机自启\n", Narrow(target + L"\\" + kWatcher).c_str());
    P("  taskkill /IM %s /F   临时停掉\n\n", Narrow(kWatcher).c_str());
    P("自检（不需要管理员）：\n");
    P("  %s --check\n", Narrow(target + L"\\rvtvdiag.exe").c_str());
    P("  也可验证 Shell 是否真能取到值：--store <某个 .rvt 路径>\n\n");
    P("属性值有缓存，首次显示可能要几秒。\n\n");
    P("卸载：设置 -> 应用 -> 找到「RVT-V」-> 卸载\n");
    P("      或运行 %s\n", Narrow(target + L"\\" + kUninstExe).c_str());

    return 0;
}

// =====================================================================
// 卸载
// =====================================================================
static int DoUninstall()
{
    const std::wstring target = ProgramFilesDir() + L"\\RvtVersion";
    const std::wstring pdPath = target + L"\\" + kPropDesc;

    P("\n=== RVT-V 卸载 ===\n\n");

    P("[1/7] 停止资源管理器\n");
    KillExplorer();
    Sleep(1200);

    P("[2/7] 摘除文件类型处理器\n");
    {
        for (size_t i = 0; i < kExtCount; i++)
        {
            std::wstring ext = kExts[i];
            std::wstring seKey = L"SOFTWARE\\Classes\\" + ext + L"\\ShellEx\\" + kShellEx;

            std::wstring got;
            if (RegGetStr(HKEY_LOCAL_MACHINE, seKey, nullptr, got) &&
                _wcsicmp(got.c_str(), kClsid) == 0)
            {
                RegDeleteKeyW(HKEY_LOCAL_MACHINE, seKey.c_str());
                P("      %-6ls 已摘除\n", ext.c_str());
            }
            else
            {
                P("      %-6ls 不是本工具，跳过\n", ext.c_str());
            }

            // 摘掉 PropertyHandlers 项（只删自己的）
            std::wstring phKey = std::wstring(
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers")
                + L"\\" + ext;
            std::wstring gotPh;
            if (RegGetStr(HKEY_LOCAL_MACHINE, phKey, nullptr, gotPh, KEY_WOW64_64KEY) &&
                _wcsicmp(gotPh.c_str(), kClsid) == 0)
            {
                HKEY k = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, phKey.c_str(), 0,
                                  KEY_SET_VALUE | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS)
                {
                    RegDeleteValueW(k, nullptr);
                    RegCloseKey(k);
                }
                SHDeleteKeyW(HKEY_LOCAL_MACHINE, phKey.c_str());
                P("      %-6ls PropertyHandlers 已摘除\n", ext.c_str());
            }

            // 清掉我们写过的 PreviewDetails / InfoTip，不碰别人的
            std::wstring sfa = L"SOFTWARE\\Classes\\SystemFileAssociations\\" + ext;
            std::wstring v;
            if (RegGetStr(HKEY_LOCAL_MACHINE, sfa, L"PreviewDetails", v) &&
                v.find(L"RvtVersion.") != std::wstring::npos)
            {
                HKEY k = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sfa.c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS)
                {
                    RegDeleteValueW(k, L"PreviewDetails");
                    RegCloseKey(k);
                }
            }
            if (RegGetStr(HKEY_LOCAL_MACHINE, sfa, L"InfoTip", v) &&
                v.find(L"RvtVersion.") != std::wstring::npos)
            {
                HKEY k = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sfa.c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS)
                {
                    RegDeleteValueW(k, L"InfoTip");
                    RegCloseKey(k);
                }
            }
        }
    }

    P("[3/7] 注销属性描述 schema\n");
    if (PathFileExistsW(pdPath.c_str()))
    {
        HRESULT hr = PSUnregisterPropertySchema(pdPath.c_str());
        P("      %s hr=0x%08lX\n", SUCCEEDED(hr) ? "成功" : "失败", (unsigned long)hr);
        PSRefreshPropertySchema();
    }
    else
    {
        P("      propdesc 不存在，跳过\n");
    }

    // 从默认列布局里撤掉我们的列
    P("[4/7] 恢复默认列布局\n");
    {
        const wchar_t* ftGuids[] = {
            L"{5c4f28b5-f869-4e84-8e60-f11db97c5cc7}",
            L"{7d49d726-3c21-4f05-99aa-fdc2c9474656}",
        };
        for (const auto* g : ftGuids)
        {
            if (EditColumnList(g, L"RvtVersion.ProductYear", false))
                P("      %ls 已移除\n", g);
            else
                P("      %ls 跳过\n", g);
        }
    }

    P("[5/7] 停止列监视器并取消开机启动\n");
    {
        // 先停进程：不停的话它会继续往 Bag 里写列，
        // 出现「卸载了但列还在」的怪现象。
        std::wstring kill = L"taskkill /F /IM " + std::wstring(kWatcher);
        _wsystem(kill.c_str());
        P("      监视器进程已停止\n");

        HKEY run = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                          0, KEY_SET_VALUE, &run) == ERROR_SUCCESS)
        {
            LONG rc = RegDeleteValueW(run, L"RvtColumnWatcher");
            RegCloseKey(run);
            P("      开机启动 %ls\n",
              (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND) ? L"已移除" : L"移除失败");
        }
        else
        {
            P("      开机启动 无需清理\n");
        }
    }

    P("[6/7] 删除注册表项\n");
    {
        std::wstring clsidKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsid;
        std::wstring progKey  = std::wstring(L"SOFTWARE\\Classes\\") + kProgId;
        std::wstring uninst   = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RvtVersion";

        if (KeyExists(HKEY_LOCAL_MACHINE, clsidKey)) { SHDeleteKeyW(HKEY_LOCAL_MACHINE, clsidKey.c_str()); P("      CLSID 已删除\n"); }
        if (KeyExists(HKEY_LOCAL_MACHINE, progKey))  { SHDeleteKeyW(HKEY_LOCAL_MACHINE, progKey.c_str());  P("      ProgID 已删除\n"); }
        if (KeyExists(HKEY_LOCAL_MACHINE, uninst))   { SHDeleteKeyW(HKEY_LOCAL_MACHINE, uninst.c_str());   P("      卸载项已删除\n"); }
    }

    P("[7/7] 删除文件\n");
    {
        // 先删能删的，删不掉的登记到重启后删除
        const wchar_t* files[] = { kDllName, kPropDesc, kSchemaTool, kDiagTool, kWatcher };
        for (size_t i = 0; i < _countof(files); i++)
        {
            std::wstring f = target + L"\\" + files[i];
            if (!DeleteFileW(f.c_str()))
                MoveFileExW(f.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }

        std::wstring self = target + L"\\" + kUninstExe;
        if (!DeleteFileW(self.c_str()))
            MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

        RemoveDirectoryW(target.c_str());
        P("      完成\n");
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    P("\n[*] 重启资源管理器\n");
    RestartExplorer();

    P("\n=== 卸载完成 ===\n");
    P("如果某些文件提示被占用，会在下次重启后自动删除。\n");
    return 0;
}

// =====================================================================
// 入口
// =====================================================================
int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(kAppName);

    // MinGW 的窄 printf 默认按 "C" locale 处理 %ls，
    // 遇到非 ASCII 就会在第一个中文字符处截断 ——
    // 实测 "Generic  (通用文件夹)" 只输出 "Generic  ("。
    // 必须切到 UTF-8 locale，%ls 才会完整输出。
    setlocale(LC_ALL, ".UTF-8");

    // 日志落到 %TEMP%\RvtVersionSetup.log，提权后的子进程也能写。
    // 用追加模式：父进程与提权后的子进程是两个独立进程，都会往同一个文件写，
    // 若用 "w" 会互相截断（之前排查时就踩到，日志只剩 3 字节 BOM）。
    {
        wchar_t tmp[MAX_PATH] = { 0 };
        GetTempPathW(MAX_PATH, tmp);
        std::wstring logPath = std::wstring(tmp) + L"RvtVersionSetup.log";
        // 不用 ccs=UTF-8：MinGW 的 msvcrt 对该模式支持不完整，会写不进去。
        _wfopen_s(&g_log, logPath.c_str(), L"a");
        if (g_log)
        {
            P("\n===== %ls =====\n", IsAdmin() ? L"[elevated] run" : L"[normal] run");
        }
        else
        {
            std::string p = Narrow(logPath);
            printf("[!] cannot open log %s\n", p.c_str());
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    bool wantUninstall = false;
    bool wantExtract   = false;
    bool wantFixCols   = false;
    bool wantDelCols   = false;
    std::wstring extractDir;

    for (int i = 1; i < argc; i++)
    {
        std::wstring a = argv[i];
        if (_wcsicmp(a.c_str(), L"/uninstall") == 0 || _wcsicmp(a.c_str(), L"-uninstall") == 0 ||
            _wcsicmp(a.c_str(), L"--uninstall") == 0)
            wantUninstall = true;
        else if (_wcsicmp(a.c_str(), L"/quiet") == 0 || _wcsicmp(a.c_str(), L"/s") == 0 ||
                 _wcsicmp(a.c_str(), L"/silent") == 0)
            g_quiet = true;
        else if (_wcsicmp(a.c_str(), L"/extract") == 0)
        {
            wantExtract = true;
            if (i + 1 < argc) extractDir = argv[++i];
        }
        else if (_wcsicmp(a.c_str(), L"/columns") == 0 || _wcsicmp(a.c_str(), L"/fix-cols") == 0)
            wantFixCols = true;
        else if (_wcsicmp(a.c_str(), L"/remove-columns") == 0)
            wantDelCols = true;
        else if (_wcsicmp(a.c_str(), L"/?") == 0 || _wcsicmp(a.c_str(), L"/help") == 0)
        {
            P("RVT-V 安装程序\n\n");
            P("  RvtVersionSetup.exe                 安装\n");
            P("  RvtVersionSetup.exe /uninstall      卸载\n");
            P("  RvtVersionSetup.exe /quiet          静默\n");
            P("  RvtVersionSetup.exe /extract <dir>  仅释放载荷（自检）\n");
            P("  RvtVersionSetup.exe /columns        写入默认列布局\n");
            P("  RvtVersionSetup.exe /remove-columns 撤销默认列布局\n");
            CoUninitialize();
            return 0;
        }
    }

    // ---- 仅释放载荷（不需要管理员）----
    if (wantExtract)
    {
        if (extractDir.empty())
        {
            P("[x] /extract 需要一个目标目录\n");
            CoUninitialize();
            return 1;
        }
        CreateDirectoryW(extractDir.c_str(), nullptr);
        P("\n释放到: %ls\n", extractDir.c_str());
        int n = ExtractAll(extractDir);
        // 载荷数量会随版本增加，不再硬编码 4
        P("\n释放 %d / %d\n", n, kPayloadCount);
        CoUninitialize();
        return n == kPayloadCount ? 0 : 1;
    }

    // ---- 需要管理员 ----
    if (!IsAdmin())
    {
        P("[*] 需要管理员权限，正在请求提权...\n");
        P("    如弹出 UAC 请点「是」\n");

        std::wstring args;
        if (wantUninstall)  args = L"/uninstall";
        else if (wantFixCols) args = L"/columns";
        else if (wantDelCols) args = L"/remove-columns";
        if (g_quiet)
        {
            if (!args.empty()) args += L" ";
            args += L"/quiet";
        }

        bool ok = RelaunchElevated(args);
        if (!ok)
        {
            P("\n[x] 提权失败或被取消。\n");
            P("    请右键本文件 -> 「以管理员身份运行」\n");
            WaitKey();
        }
        CoUninitialize();
        return ok ? 0 : 1;
    }

    int rc;
    if (wantUninstall)    rc = DoUninstall();
    else if (wantFixCols) rc = DoFixColumns(true);
    else if (wantDelCols) rc = DoFixColumns(false);
    else                  rc = DoInstall();

    if (rc == 0) WaitKey();
    else { P("\n未完成，请查看上方错误。\n"); WaitKey(); }

    CoUninitialize();
    return rc;
}
