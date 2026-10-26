// =====================================================================
// RvtVersion 属性处理器 (Property Handler) — 纯 Win32 / COM 实现
//
// 作用：让资源管理器「详细信息」视图的列里直接显示 Revit 版本。
//      这是唯一能在列表列中显示自定义值的正规机制。
//      微软明确：property handler 不能用托管代码实现
//      （property handlers cannot be implemented in managed code）。
//
// 设计要点：
//   - 不加载模型，只读 CFB 头 + 目录扇区 + BasicFileInfo 流（通常 < 10 KB）。
//   - 实现 IInitializeWithStream（资源管理器主路径，已缓存文件句柄），
//     同时实现 IInitializeWithFile 兜底。
//   - 属性集用自定义 FMTID，propID 从 2 起，避免与系统属性冲突。
//   - 解析走语义识别而非固定偏移，见 BasicFileInfo.h 的说明。
// =====================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <propsys.h>
#include <propvarutil.h>
#include <new>
#include <string>
#include <vector>
#include <stdint.h>

#include "PropKeys.h"
#include "RevitCfb.h"
#include "BasicFileInfo.h"

// {5AD3E9FC-A720-47F6-8DFA-9AACD3286E01}
static const CLSID CLSID_RvtVersionPropertyHandler =
    { 0x5ad3e9fc, 0xa720, 0x47f6, { 0x8d, 0xfa, 0x9a, 0xac, 0xd3, 0x28, 0x6e, 0x01 } };

static HINSTANCE g_hInst = nullptr;
static LONG g_cDllRef = 0;

// ---------------------------------------------------------------------
// 格式号 → 展示年份
//
// 2016 及之后 1:1；更早的格式号与商业版本号不重合（9.0/10.0/…）。
// 刻意分两个输出：
//   yearOut    → "2012"          （排序 / 短标签 / 图标键用）
//   productOut → "12.0 / 2012"   （人工识别用）
// 曾经混用导致图标文件名变成 rvtv_1502012.ico。
// ---------------------------------------------------------------------
static void ResolveYear(int fmt,
                        wchar_t* yearOut, size_t yearCap,
                        wchar_t* productOut, size_t productCap)
{
    struct Map { int fmt; const wchar_t* product; const wchar_t* year; };
    static const Map kMap[] = {
        { 2004, L"6.0 / 2004",  L"2004" }, { 2005, L"7.0 / 2005",  L"2005" },
        { 2006, L"8.0 / 2006",  L"2006" }, { 2007, L"8.1 / 2007",  L"2007" },
        { 2008, L"9.0 / 2008",  L"2008" }, { 2009, L"9.1 / 2009",  L"2009" },
        { 2010, L"10.0 / 2010", L"2010" }, { 2011, L"11.0 / 2011", L"2011" },
        { 2012, L"12.0 / 2012", L"2012" }, { 2013, L"13.0 / 2013", L"2013" },
        { 2014, L"14.0 / 2014", L"2014" },
    };

    for (const auto& m : kMap)
    {
        if (m.fmt == fmt)
        {
            if (yearOut)    wcsncpy_s(yearOut, yearCap, m.year, _TRUNCATE);
            if (productOut) wcsncpy_s(productOut, productCap, m.product, _TRUNCATE);
            return;
        }
    }

    wchar_t buf[32];
    swprintf_s(buf, L"%d", fmt);
    if (yearOut)    wcsncpy_s(yearOut, yearCap, buf, _TRUNCATE);
    if (productOut) wcsncpy_s(productOut, productCap, buf, _TRUNCATE);
}

// ---------------------------------------------------------------------
// 探测：从 CFB 读取 BasicFileInfo 并解析
// ---------------------------------------------------------------------
struct ProbeResult
{
    bfi::Meta meta;
    bool      isCfb = false;
    bool      hasStream = false;
};

template <typename Reader>
static void ProbeInternal(Reader& reader, ProbeResult& result)
{
    if (!reader.Open()) return;      // 不是 CFB，或结构损坏
    result.isCfb = true;

    std::vector<uint8_t> info;
    if (reader.ReadStream(L"BasicFileInfo", info) && !info.empty())
    {
        result.hasStream = true;
        bfi::Parse(info, result.meta);
        if (result.meta.ok) return;
    }

    // 无 BasicFileInfo（早期 .rft 模板）：读文件头做兜底扫描
    std::vector<uint8_t> head;
    if (reader.ReadHead(128 * 1024, head) && !head.empty())
        bfi::Parse(head, result.meta);
}

// ---------------------------------------------------------------------
// 诊断日志
//
// 资源管理器调用 handler 是黑盒：列空白时无法判断是「没被调用」还是
// 「被调用了但返回空」。设环境变量 RVTV_LOG=1 后，每次 Initialize /
// GetValue 都会追加到 %TEMP%\RvtVersionHandler.log。
// 默认关闭，只有诊断时开，不影响正常性能。
// ---------------------------------------------------------------------
static void DiagLog(const char* fmt, ...)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        wchar_t v[8] = { 0 };
        enabled = (GetEnvironmentVariableW(L"RVTV_LOG", v, 8) > 0) ? 1 : 0;
    }
    if (!enabled) return;

    wchar_t tmp[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"RvtVersionHandler.log";

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"a");
    if (!f) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d pid=%lu] %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            (unsigned long)GetCurrentProcessId(), buf);
    fclose(f);
}

// ---------------------------------------------------------------------
// 属性处理器
// ---------------------------------------------------------------------
class RvtVersionPropertyHandler final
    : public IPropertyStore
    , public IInitializeWithStream
    , public IInitializeWithFile
    , public IPropertyStoreCapabilities
    , public IPropertyStoreFactory
{
public:
    RvtVersionPropertyHandler() : _cRef(1), _stream(nullptr)
    {
        InterlockedIncrement(&g_cDllRef);
    }

    // ---- IUnknown ----
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;

        // 多继承下各接口基类指针值不同，必须按实际接口做转换。
        // 曾经把 IPropertyStoreFactory 也 static_cast 成 IPropertyStore*：
        // Shell 拿到错位的 vtable，调 GetPropertyStore 会落到 GetCount 上，
        // 参数被当作 DWORD* 写入，属于未定义行为。
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IPropertyStore))
            *ppv = static_cast<IPropertyStore*>(this);
        else if (IsEqualIID(riid, IID_IPropertyStoreFactory))
            *ppv = static_cast<IPropertyStoreFactory*>(this);
        else if (IsEqualIID(riid, IID_IInitializeWithStream))
            *ppv = static_cast<IInitializeWithStream*>(this);
        else if (IsEqualIID(riid, IID_IInitializeWithFile))
            *ppv = static_cast<IInitializeWithFile*>(this);
        else if (IsEqualIID(riid, IID_IPropertyStoreCapabilities))
            *ppv = static_cast<IPropertyStoreCapabilities*>(this);
        else
            return E_NOINTERFACE;

        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&_cRef); }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        LONG c = InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return (ULONG)c;
    }

    // ---- IInitializeWithStream ----
    IFACEMETHODIMP Initialize(IStream* pstream, DWORD /*grfMode*/) override
    {
        if (!pstream) return E_INVALIDARG;
        if (_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);

        _stream = pstream;
        _stream->AddRef();

        STATSTG stat;
        ZeroMemory(&stat, sizeof(stat));
        if (SUCCEEDED(_stream->Stat(&stat, STATFLAG_NONAME)))
            _sizeBytes = stat.cbSize.QuadPart;

        CfbReader reader(_stream);
        ProbeInternal(reader, _probe);

        DiagLog("InitWithStream size=%llu cfb=%d stream=%d ok=%d fmt=%d build=%ls",
                (unsigned long long)_sizeBytes, (int)_probe.isCfb,
                (int)_probe.hasStream, (int)_probe.meta.ok,
                _probe.meta.formatVersion,
                _probe.meta.build.empty() ? L"-" : _probe.meta.build.c_str());
        return S_OK;
    }

    // ---- IInitializeWithFile ----
    IFACEMETHODIMP Initialize(LPCWSTR pszFilePath, DWORD grfMode) override
    {
        if (!pszFilePath) return E_INVALIDARG;
        if (_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        (void)grfMode;

        // 允许共享读写删除：文件可能正被 Revit 打开
        HANDLE h = CreateFileW(pszFilePath, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            DiagLog("InitWithFile OPEN FAILED err=%lu path=%ls",
                    GetLastError(), pszFilePath);
            return S_OK;   // 打不开就返回空属性，不抛错
        }

        LARGE_INTEGER sz;
        ZeroMemory(&sz, sizeof(sz));
        if (GetFileSizeEx(h, &sz)) _sizeBytes = (ULONGLONG)sz.QuadPart;

        CfbReader reader(h);
        ProbeInternal(reader, _probe);
        CloseHandle(h);

        DiagLog("InitWithFile size=%llu cfb=%d stream=%d ok=%d fmt=%d build=%ls path=%ls",
                (unsigned long long)_sizeBytes, (int)_probe.isCfb,
                (int)_probe.hasStream, (int)_probe.meta.ok,
                _probe.meta.formatVersion,
                _probe.meta.build.empty() ? L"-" : _probe.meta.build.c_str(),
                pszFilePath);
        return S_OK;
    }

    // ---- IPropertyStore ----
    IFACEMETHODIMP GetCount(DWORD* pcProps) override
    {
        if (!pcProps) return E_POINTER;
        *pcProps = _probe.meta.ok ? (DWORD)kPropCount : 0;
        DiagLog("GetCount -> %lu (ok=%d)", (unsigned long)*pcProps, (int)_probe.meta.ok);
        return S_OK;
    }

    IFACEMETHODIMP GetAt(DWORD iProp, PROPERTYKEY* pkey) override
    {
        if (!pkey) return E_POINTER;
        if (!_probe.meta.ok || iProp >= kPropCount) return E_INVALIDARG;
        *pkey = kProps[iProp].key;
        return S_OK;
    }

    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) override
    {
        if (!pv) return E_POINTER;
        PropVariantInit(pv);
        if (!_probe.meta.ok) return S_OK;      // 空值，不是错误

        for (size_t i = 0; i < kPropCount; i++)
        {
            if (IsEqualPropertyKey(key, kProps[i].key))
            {
                FillValue(i, pv);
                return S_OK;
            }
        }
        return S_OK;
    }

    IFACEMETHODIMP SetValue(REFPROPERTYKEY, REFPROPVARIANT) override
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    IFACEMETHODIMP Commit() override
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    // ---- IPropertyStoreCapabilities ----
    IFACEMETHODIMP IsPropertyWritable(REFPROPERTYKEY) override { return S_FALSE; }

    // ---- IPropertyStoreFactory ----
    // 资源管理器若要求按用途返回不同 store，这里统一返回自身即可
    IFACEMETHODIMP GetPropertyStore(GETPROPERTYSTOREFLAGS, IUnknown*,
                                    REFIID riid, void** ppv) override
    {
        return QueryInterface(riid, ppv);
    }

    IFACEMETHODIMP GetPropertyStoreForKeys(const PROPERTYKEY*, UINT, GETPROPERTYSTOREFLAGS,
                                           REFIID riid, void** ppv) override
    {
        return QueryInterface(riid, ppv);
    }

private:
    ~RvtVersionPropertyHandler()
    {
        if (_stream) _stream->Release();
        InterlockedDecrement(&g_cDllRef);
    }

    void FillValue(size_t index, PROPVARIANT* pv)
    {
        const bfi::Meta& m = _probe.meta;
        switch (index)
        {
        case 0:
            InitPropVariantFromUInt32((ULONG)m.formatVersion, pv);
            break;
        case 1:
        {
            wchar_t year[16] = { 0 }, product[64] = { 0 };
            ResolveYear(m.formatVersion, year, _countof(year), product, _countof(product));
            InitPropVariantFromString(product, pv);
            break;
        }
        case 2:
        {
            wchar_t year[16] = { 0 }, product[64] = { 0 };
            ResolveYear(m.formatVersion, year, _countof(year), product, _countof(product));
            InitPropVariantFromString(year, pv);
            break;
        }
        case 3: InitPropVariantFromString(m.build.c_str(), pv);       break;
        case 4: InitPropVariantFromString(m.locale.c_str(), pv);      break;
        case 5: InitPropVariantFromString(m.worksharing.c_str(), pv); break;
        case 6: InitPropVariantFromString(m.savedPath.c_str(), pv);   break;
        case 7: InitPropVariantFromUInt64(_sizeBytes, pv);            break;
        default: break;
        }
    }

    LONG           _cRef;
    IStream*       _stream;
    ULONGLONG      _sizeBytes = 0;
    ProbeResult    _probe;
};

// ---------------------------------------------------------------------
// IClassFactory
// ---------------------------------------------------------------------
class RvtVersionClassFactory final : public IClassFactory
{
public:
    RvtVersionClassFactory() : _cRef(1) { InterlockedIncrement(&g_cDllRef); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
        {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&_cRef); }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        LONG c = InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return (ULONG)c;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;

        RvtVersionPropertyHandler* handler = new (std::nothrow) RvtVersionPropertyHandler();
        if (!handler) return E_OUTOFMEMORY;

        HRESULT hr = handler->QueryInterface(riid, ppv);
        handler->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL fLock) override
    {
        if (fLock) InterlockedIncrement(&g_cDllRef);
        else       InterlockedDecrement(&g_cDllRef);
        return S_OK;
    }

private:
    ~RvtVersionClassFactory() { InterlockedDecrement(&g_cDllRef); }
    LONG _cRef;
};

// ---------------------------------------------------------------------
// DLL 导出
// ---------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hInst = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, CLSID_RvtVersionPropertyHandler)) return CLASS_E_CLASSNOTAVAILABLE;

    RvtVersionClassFactory* factory = new (std::nothrow) RvtVersionClassFactory();
    if (!factory) return E_OUTOFMEMORY;

    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return (g_cDllRef == 0) ? S_OK : S_FALSE;
}
