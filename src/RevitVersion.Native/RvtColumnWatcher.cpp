// =====================================================================
// RvtColumnWatcher.cpp —— 常驻监视器
//
// 任务：让「详细信息」视图里的 Revit Version 列只在含 Revit 文件的文件夹出现。
//
// 为什么需要常驻进程（实测结论，不是设计偏好）：
//   Shell 的列布局只有两个声明式来源：
//     (a) FolderTypes\<type>\TopViews\...\ColumnList —— 按文件夹类型，全局生效；
//     (b) Bags\<slot>\Shell\<type>\ColInfo —— 按具体文件夹，二进制私有格式。
//   两者都无法表达「取决于文件夹内容」这个条件。
//   实测写 HKCU 的 FolderTypes 覆盖确实被 Shell 采用
//   （--view-cols 在真实视图里能看到 RvtVersion.ProductYear），
//   但 C:\Windows 这类无关文件夹也会多出一列空值 —— 不满足需求。
//   要做到条件化，只能在该文件夹的视图活着时用 IColumnManager::SetColumns
//   实时增删；Shell 会把这个状态写回该文件夹自己的 Bag
//   （已实测：操作后 5 个 Bag 的 ColInfo 里出现我们的 FMTID），
//   所以关掉重开还会记得。
//
// 触发策略：
//   轮询而不是 hook。理由：
//     - 没有公开 API 能订阅「资源管理器导航到某文件夹」；
//     - 未公开的 Shell 事件（SHChangeNotify 的 SHCNE_* ）不覆盖导航；
//     - 轮询 700ms 一次，实际 CPU 占用可忽略（每次只是枚举窗口 + 
//       FindFirstFile 探测 4 个通配符）。
//   只在「窗口集合 / 每个窗口的路径」与上次不同时才真正去碰 Shell，
//   避免无谓 SetColumns 导致视图反复重排。
//
// 用法：
//   rvtvwatch.exe            前台运行（Ctrl+C 退出）
//   rvtvwatch.exe --install  注册开机启动（HKCU Run）
//   rvtvwatch.exe --remove   取消开机启动
//   rvtvwatch.exe --once     只同步一次就退出（给安装器/排障用）
//   rvtvwatch.exe --status   打印当前各窗口状态后退出
//
// ---------------------------------------------------------------------
// 为什么本程序链接为 GUI 子系统（Subsystem=2），而不是控制台子系统
// ---------------------------------------------------------------------
// 症状：开机自启后桌面出现一个 CMD 黑窗口；用户关掉它，Revit 列就失效了。
//
// 根因（实测 PE 头）：Subsystem = 3 (IMAGE_SUBSYSTEM_WINDOWS_CUI)。
//   HKCU\...\Run 启动控制台程序时，系统会为它分配一个控制台窗口 —— 这就是
//   那个黑窗口；而那个窗口承载的正是监视器自身，关窗 = 结束进程 = 列失效。
//   两个症状是同一个根因。
//
// 修复：链接时加 -Wl,--subsystem,windows，Subsystem 变 2
//   (IMAGE_SUBSYSTEM_WINDOWS_GUI)。GUI 程序不分配控制台，因此永久无窗口，
//   也就没有可被误关的东西。
//
// 代价与对策：GUI 子系统下没有控制台，printf 一类仍可用但要自己处理句柄。
//   实测（MinGW-w64 + -static，见 README 8.6）：
//     - wmain(int, wchar_t**) 在 GUI 子系统下依然是有效入口，不需要改 WinMain；
//     - 从 bash/cmd 启动时，继承来的 stdout 句柄在 CRT 里是好的，
//       GetStdHandle(STD_OUTPUT_HANDLE) 能拿到有效 HANDLE；
//     - WriteFile() 直写该句柄，管道 / 文件重定向 / 附着控制台三种情形都正常；
//     - 反过来，_open_osfhandle + _dup2(h, 1) 会以 errno=9 (EBADF) 失败，
//       并且会破坏已有的 stdout —— 所以这里绝不做「恢复标准流」的动作。
//   Out() 因此改成 Win32 WriteFile 直写，完全绕开 CRT 的 fd 层。
//   开机自启没有重定向，句柄无效时 Out() 静默丢弃，不产生任何 UI。
// =====================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <cstdio>
#include <string>
#include <vector>
#include <map>

#include "ShellColumnSync.h"

static const wchar_t* kRunKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"RvtColumnWatcher";

// ---------------------------------------------------------------- 输出
// 用 Win32 WriteFile 直写句柄，而不是 fwrite/printf。
// 理由见文件头：GUI 子系统下 CRT 的 fd 层不适合做重定向，而句柄层是可靠的。
// 没有控制台也没有重定向时（开机自启的常态），句柄无效，静默丢弃。
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

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;

    DWORD wrote = 0;
    WriteFile(h, s.data(), (DWORD)s.size(), &wrote, nullptr);
}

// 是否有可写的控制台/重定向。GUI 子系统下开机自启时为 false。
static bool HasConsoleOut()
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    DWORD t = GetFileType(h);
    return t == FILE_TYPE_CHAR || t == FILE_TYPE_PIPE || t == FILE_TYPE_DISK;
}

static std::wstring ExePath()
{
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, _countof(buf));
    return buf;
}

// ---------------------------------------------------------------- 开机启动
static bool InstallAutostart()
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;

    std::wstring cmd = L"\"" + ExePath() + L"\" --silent";
    LONG rc = RegSetValueExW(k, kRunName, 0, REG_SZ,
                             (const BYTE*)cmd.c_str(),
                             (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

static bool RemoveAutostart()
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return false;
    LONG rc = RegDeleteValueW(k, kRunName);
    RegCloseKey(k);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

static bool AutostartInstalled()
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;

    wchar_t buf[MAX_PATH * 3] = {};
    DWORD cb = sizeof(buf), type = 0;
    LONG rc = RegQueryValueExW(k, kRunName, nullptr, &type, (BYTE*)buf, &cb);
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

// ---------------------------------------------------------------- 单轮同步
// 返回本次真正动过列的窗口数。
static size_t OneRound(bool verbose)
{
    std::vector<WindowSyncInfo> infos;
    size_t handled = colsync::SyncAllWindows(&infos);

    size_t changed = 0;
    for (const auto& i : infos)
        if (i.action == SyncAction::Added || i.action == SyncAction::Removed)
            changed++;

    if (verbose)
    {
        Out(L"  窗口 %zu 个（文件系统视图）\n", handled);
        for (const auto& i : infos)
        {
            const wchar_t* act =
                i.action == SyncAction::Added   ? L"补上列" :
                i.action == SyncAction::Removed ? L"去掉列" :
                i.action == SyncAction::Failed  ? L"失败  " : L"不变  ";

            Out(L"    [%ls] %ls  含Revit=%ls 列可见=%ls\n",
                act, i.path.c_str(),
                i.hasRevit ? L"是" : L"否",
                i.columnVisible ? L"是" : L"否");

            if (i.action == SyncAction::Failed)
                Out(L"         hr=0x%08lX\n", (unsigned long)i.hr);
        }
        Out(L"  本次改动 %zu 个窗口\n", changed);
    }

    return changed;
}

// ---------------------------------------------------------------- 轮询
// 只在「窗口集合或其路径」变化时才碰 Shell。
// SetColumns 会让 Explorer 重排视图（肉眼可见的闪烁），
// 无条件每轮都调会很难看，所以必须去重。
struct WinSig
{
    std::wstring path;
    bool         hasRevit = false;

    bool operator==(const WinSig& o) const
    {
        return _wcsicmp(path.c_str(), o.path.c_str()) == 0 && hasRevit == o.hasRevit;
    }
};

static std::vector<WinSig> CollectSignatures()
{
    std::vector<WinSig> sigs;

    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_IShellWindows, (void**)&sw)) || !sw)
        return sigs;

    long count = 0;
    sw->get_Count(&count);

    for (long i = 0; i < count; i++)
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
            WinSig s;
            s.path = colsync::WindowFolderPath(sb);
            if (!s.path.empty())
            {
                s.hasRevit = colsync::FolderHasRevitFiles(s.path);
                sigs.push_back(s);
            }
            sb->Release();
        }
        disp->Release();
    }

    sw->Release();
    return sigs;
}

static bool SameSigs(const std::vector<WinSig>& a, const std::vector<WinSig>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (!(a[i] == b[i])) return false;
    return true;
}

// ---------------------------------------------------------------- main
int wmain(int argc, wchar_t** argv)
{
    // GUI 子系统下没有控制台，SetConsoleOutputCP 会失败（无害）。
    // 只在真有可能存在控制台时才调，避免无意义的失败调用。
    if (HasConsoleOut()) SetConsoleOutputCP(CP_UTF8);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    bool silent = false, once = false, statusOnly = false;

    for (int i = 1; i < argc; i++)
    {
        std::wstring a = argv[i];
        if (_wcsicmp(a.c_str(), L"--silent") == 0)        silent = true;
        else if (_wcsicmp(a.c_str(), L"--once") == 0)     once = true;
        else if (_wcsicmp(a.c_str(), L"--status") == 0)   statusOnly = true;
        else if (_wcsicmp(a.c_str(), L"--install") == 0)
        {
            bool ok = InstallAutostart();
            Out(ok ? L"[+] 已注册开机启动\n" : L"[x] 注册失败\n");
            CoUninitialize();
            return ok ? 0 : 1;
        }
        else if (_wcsicmp(a.c_str(), L"--remove") == 0)
        {
            bool ok = RemoveAutostart();
            Out(ok ? L"[+] 已取消开机启动\n" : L"[x] 取消失败\n");
            CoUninitialize();
            return ok ? 0 : 1;
        }
        else if (_wcsicmp(a.c_str(), L"/?") == 0 || _wcsicmp(a.c_str(), L"--help") == 0)
        {
            Out(L"rvtvwatch —— 按文件夹内容自动显示 Revit Version 列\n\n");
            Out(L"  rvtvwatch.exe            前台运行（Ctrl+C 退出）\n");
            Out(L"  rvtvwatch.exe --silent   后台运行，不打印\n");
            Out(L"  rvtvwatch.exe --once     只同步一次就退出\n");
            Out(L"  rvtvwatch.exe --status   打印当前状态后退出\n");
            Out(L"  rvtvwatch.exe --install  注册开机启动\n");
            Out(L"  rvtvwatch.exe --remove   取消开机启动\n");
            CoUninitialize();
            return 0;
        }
    }

    if (statusOnly)
    {
        Out(L"\n=== 当前窗口状态 ===\n");
        OneRound(true);
        Out(L"  开机启动: %ls\n", AutostartInstalled() ? L"已注册" : L"未注册");
        CoUninitialize();
        return 0;
    }

    if (once)
    {
        if (!silent) Out(L"\n=== 同步一次 ===\n");
        OneRound(!silent);
        CoUninitialize();
        return 0;
    }

    // ---- 常驻轮询 ----
    if (!silent)
    {
        Out(L"rvtvwatch 已启动（Ctrl+C 退出）\n");
        Out(L"  规则: 文件夹含 Revit 文件 -> 显示 Revit Version 列\n");
        Out(L"        不含            -> 隐藏该列\n");
        Out(L"  轮询间隔 700ms，仅在窗口/路径变化时才动 Shell\n\n");
    }

    // 退出条件有两类：
    //   1) Ctrl+C / 关闭控制台 —— 只在真有控制台时才有意义，GUI 子系统下
    //      SetConsoleCtrlHandler 也能注册成功但永远不会被触发，无副作用；
    //   2) 单实例互斥体被放弃 —— 见下。
    static volatile bool g_stop = false;
    auto handler = [](DWORD) -> BOOL { g_stop = true; return TRUE; };
    if (HasConsoleOut())
        SetConsoleCtrlHandler((PHANDLER_ROUTINE)+handler, TRUE);

    // 单实例：避免用户重复点击、或安装器与 Run 键各起一份导致多个监视器
    // 同时 SetColumns 互相打架（表现为列闪烁）。
    // 第二份实例创建互斥体失败，直接退出。
    // 用 Local\ 前缀：每个登录会话一份即可，不跨会话。
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"Local\\RvtColumnWatcher.SingleInstance");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (!silent) Out(L"[!] 已有监视器实例在运行，本实例退出\n");
        CloseHandle(hMutex);
        CoUninitialize();
        return 0;
    }

    std::vector<WinSig> last;
    bool first = true;

    while (!g_stop)
    {
        std::vector<WinSig> cur = CollectSignatures();

        // 窗口集合变了、或某个窗口换了文件夹、或内容变化（新建/删除 rvt），
        // 才真正去同步。首次进来无条件跑一轮。
        if (first || !SameSigs(cur, last))
        {
            first = false;
            last = cur;

            std::vector<WindowSyncInfo> infos;
            colsync::SyncAllWindows(&infos);

            if (!silent)
            {
                for (const auto& i : infos)
                {
                    if (i.action == SyncAction::Added)
                        Out(L"  [+] 显示列: %ls\n", i.path.c_str());
                    else if (i.action == SyncAction::Removed)
                        Out(L"  [-] 隐藏列: %ls\n", i.path.c_str());
                    else if (i.action == SyncAction::Failed)
                        Out(L"  [x] 失败 0x%08lX: %ls\n",
                            (unsigned long)i.hr, i.path.c_str());
                }
            }
        }

        Sleep(700);
    }

    if (!silent) Out(L"\n已退出\n");
    if (hMutex) CloseHandle(hMutex);
    CoUninitialize();
    return 0;
}
