// =====================================================================
// ShellColumnSync.h —— 「按文件夹内容自动显示 Revit Version 列」核心逻辑
//
// 为什么必须是常驻进程：
//   Windows Shell 的列布局只有两种声明式来源 ——
//     (a) FolderTypes\<type>\TopViews\...\ColumnList：按文件夹类型，全局；
//     (b) Bags\<slot>\Shell\<type>\ColInfo：按具体文件夹，二进制私有格式。
//   没有任何一处能表达「这个文件夹里有没有某类文件」这个条件。
//   实测：写 HKCU 的 FolderTypes 覆盖确实被 Shell 采用（--view-cols 能在
//   真实视图里看到 RvtVersion.ProductYear），但它是按类型全局生效的，
//   C:\Windows 这种无关文件夹也会多出一列空值。
//   要做到用户要的条件语义，只能在该文件夹的视图活着时，通过
//   IColumnManager::SetColumns 实时增删 —— Shell 会把它写回该文件夹自己的
//   Bag（已实测 5 个 Bag 里出现我们的 FMTID），所以关掉重开也记得。
//
// 关键约束与实测依据：
//   - SetColumns 只在「真实的资源管理器窗口」上会持久化；
//     自己 CoCreateInstance 的 IExplorerBrowser 用独立 property bag，
//     改完 SetColumns 返回 S_OK 但关掉就不见了（已实测 --set-cols）。
//   - 取 IShellBrowser 不能从 IDispatch 直接 QI（E_NOINTERFACE），
//     必须 QI IServiceProvider 再 QueryService(SID_STopLevelBrowser)。
//   - 文件夹路径要从 IFolderView2::GetFolder(IID_IPersistFolder2)
//     -> GetCurFolder 拿 PIDL 再 SHGetPathFromIDList，比读窗口标题可靠。
// =====================================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <propsys.h>
#include <propvarutil.h>
#include <string>
#include <vector>

#include "PropKeys.h"

// Shell 的「每文件夹增删列」结果
enum class SyncAction
{
    None,       // 状态已正确，无需动
    Added,      // 补上了这一列
    Removed,    // 去掉了这一列
    Failed,     // 调 Shell 失败
};

struct WindowSyncInfo
{
    std::wstring path;          // 窗口的文件夹路径（空 = 非文件系统视图）
    bool         hasRevit = false;
    bool         columnVisible = false;
    SyncAction   action = SyncAction::None;
    HRESULT      hr = S_OK;
};

namespace colsync
{

// 判断文件夹直接子项里有没有 Revit 文件（.rvt/.rfa/.rte/.rft）。
// 只看直接子项，与「详细信息视图里能直接看到该列」的语义一致；
// 递归会让子文件夹很多的目录每次扫描都变慢。
inline bool FolderHasRevitFiles(const std::wstring& dir)
{
    static const wchar_t* kPatterns[] = { L"\\*.rvt", L"\\*.rfa", L"\\*.rte", L"\\*.rft" };

    for (const auto* pat : kPatterns)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + pat).c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            FindClose(h);
            return true;
        }
    }
    return false;
}

inline std::wstring WindowFolderPath(IShellBrowser* sb)
{
    IShellView* sv = nullptr;
    if (FAILED(sb->QueryActiveShellView(&sv)) || !sv) return L"";

    std::wstring out;
    IFolderView2* fv = nullptr;
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

// 把某个窗口的 Revit Version 列调整到「应该有的状态」：
//   文件夹含 Revit 文件 -> 该列可见
//   文件夹不含           -> 该列隐藏
// 只在该窗口确实需要变化时才调 SetColumns，避免无谓写 Bag（每次写都会让
// Explorer 重排视图，频繁调用会闪）。
inline WindowSyncInfo SyncWindow(IShellBrowser* sb)
{
    WindowSyncInfo info;

    info.path = WindowFolderPath(sb);
    if (info.path.empty()) return info;             // 非文件系统视图（此电脑、回收站等）

    info.hasRevit = FolderHasRevitFiles(info.path);

    IShellView* sv = nullptr;
    if (FAILED(sb->QueryActiveShellView(&sv)) || !sv)
    {
        info.hr = E_FAIL;
        return info;
    }

    IFolderView2* fv = nullptr;
    if (FAILED(sv->QueryInterface(IID_IFolderView2, (void**)&fv)) || !fv)
    {
        sv->Release();
        info.hr = E_NOINTERFACE;
        return info;
    }

    IColumnManager* cm = nullptr;
    if (FAILED(fv->QueryInterface(IID_IColumnManager, (void**)&cm)) || !cm)
    {
        fv->Release();
        sv->Release();
        info.hr = E_NOINTERFACE;
        return info;
    }

    // 只有在「详细信息」视图下动列才有意义；图标/平铺视图没有列概念，
    // 此时 GetColumnCount 常返回 0，直接跳过免得误判。
    UINT nVis = 0;
    if (SUCCEEDED(cm->GetColumnCount(CM_ENUM_VISIBLE, &nVis)) && nVis > 0)
    {
        std::vector<PROPERTYKEY> keys(nVis);
        if (SUCCEEDED(cm->GetColumns(CM_ENUM_VISIBLE, keys.data(), nVis)))
        {
            bool present = false;
            for (const auto& k : keys)
                if (IsEqualGUID(k.fmtid, FMTID_RvtVersion) && k.pid == PID_RVT_PRODUCT_YEAR)
                    present = true;

            info.columnVisible = present;

            if (info.hasRevit && !present)
            {
                std::vector<PROPERTYKEY> nk(keys);
                nk.push_back(PKEY_RvtProductYear);
                info.hr = cm->SetColumns(nk.data(), (UINT)nk.size());
                info.action = SUCCEEDED(info.hr) ? SyncAction::Added : SyncAction::Failed;
                if (SUCCEEDED(info.hr)) info.columnVisible = true;
            }
            else if (!info.hasRevit && present)
            {
                std::vector<PROPERTYKEY> nk;
                nk.reserve(keys.size());
                for (const auto& k : keys)
                    if (!IsEqualGUID(k.fmtid, FMTID_RvtVersion))
                        nk.push_back(k);
                info.hr = cm->SetColumns(nk.data(), (UINT)nk.size());
                info.action = SUCCEEDED(info.hr) ? SyncAction::Removed : SyncAction::Failed;
                if (SUCCEEDED(info.hr)) info.columnVisible = false;
            }
        }
    }

    cm->Release();
    fv->Release();
    sv->Release();
    return info;
}

// 遍历所有已打开的资源管理器窗口并同步。
// 返回处理的窗口数，out 收集每个窗口的详情（给日志用）。
inline size_t SyncAllWindows(std::vector<WindowSyncInfo>* out)
{
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_IShellWindows, (void**)&sw)) || !sw)
        return 0;

    long count = 0;
    sw->get_Count(&count);

    size_t handled = 0;
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
            WindowSyncInfo info = SyncWindow(sb);
            if (!info.path.empty())
            {
                if (out) out->push_back(info);
                handled++;
            }
            sb->Release();
        }
        disp->Release();
    }

    sw->Release();
    return handled;
}

}   // namespace colsync
