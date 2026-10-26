// =====================================================================
// 属性处理器冒烟测试
//
// 在进程内加载 DLL，走 IInitializeWithStream / IInitializeWithFile，
// 枚举 IPropertyStore 的全部属性值。
// 不需要注册 COM，不需要管理员权限。
//
// 用法：rvtvhandlertest.exe <file1> [file2] ...
// 退出码：0 全部成功；1 有失败
// =====================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <propsys.h>
#include <propvarutil.h>
#include <cstdio>
#include <string>
#include <vector>

#include "PropKeys.h"

typedef HRESULT (STDAPICALLTYPE *PFN_DllGetClassObject)(REFCLSID, REFIID, void**);

// 与 PropertyHandler.cpp 保持一致
static const CLSID CLSID_Handler =
    { 0x5ad3e9fc, 0xa720, 0x47f6, { 0x8d, 0xfa, 0x9a, 0xac, 0xd3, 0x28, 0x6e, 0x01 } };

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring GetStr(IPropertyStore* store, REFPROPERTYKEY key)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring out;
    if (SUCCEEDED(store->GetValue(key, &pv)))
    {
        PWSTR s = nullptr;
        if (SUCCEEDED(PropVariantToStringAlloc(pv, &s)) && s)
        {
            out = s;
            CoTaskMemFree(s);
        }
    }
    PropVariantClear(&pv);
    return out;
}

static ULONG GetUInt(IPropertyStore* store, REFPROPERTYKEY key)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    ULONG v = 0;
    if (SUCCEEDED(store->GetValue(key, &pv)))
        PropVariantToUInt32(pv, &v);
    PropVariantClear(&pv);
    return v;
}

// 递归收集 Revit 文件
static void ScanDirectory(const std::wstring& dir, std::vector<std::wstring>& out)
{
    WIN32_FIND_DATAW fd;
    std::wstring pattern = dir + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do
    {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            ScanDirectory(full, out);
        }
        else
        {
            const wchar_t* dot = wcsrchr(fd.cFileName, L'.');
            if (!dot) continue;
            if (_wcsicmp(dot, L".rvt") == 0 || _wcsicmp(dot, L".rfa") == 0 ||
                _wcsicmp(dot, L".rte") == 0 || _wcsicmp(dot, L".rft") == 0)
                out.push_back(full);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

static int TestFile(PFN_DllGetClassObject pfn, const wchar_t* path)
{
    IClassFactory* factory = nullptr;
    HRESULT hr = pfn(CLSID_Handler, IID_IClassFactory, (void**)&factory);
    if (FAILED(hr) || !factory)
    {
        printf("  [x] DllGetClassObject failed hr=0x%08lX\n", (unsigned long)hr);
        return 1;
    }

    IPropertyStore* store = nullptr;
    hr = factory->CreateInstance(nullptr, IID_IPropertyStore, (void**)&store);
    factory->Release();
    if (FAILED(hr) || !store)
    {
        printf("  [x] CreateInstance failed hr=0x%08lX\n", (unsigned long)hr);
        return 1;
    }

    // 主路径：IInitializeWithStream
    bool inited = false;
    IInitializeWithStream* initStream = nullptr;
    if (SUCCEEDED(store->QueryInterface(IID_IInitializeWithStream, (void**)&initStream)) && initStream)
    {
        IStream* stream = nullptr;
        if (SUCCEEDED(SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_WRITE,
                                             FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream)))
        {
            inited = SUCCEEDED(initStream->Initialize(stream, STGM_READ));
            stream->Release();
        }
        initStream->Release();
    }

    if (!inited)
    {
        IInitializeWithFile* initFile = nullptr;
        if (SUCCEEDED(store->QueryInterface(IID_IInitializeWithFile, (void**)&initFile)) && initFile)
        {
            inited = SUCCEEDED(initFile->Initialize(path, STGM_READ));
            initFile->Release();
        }
    }

    DWORD count = 0;
    store->GetCount(&count);

    ULONG fmt    = GetUInt(store, PKEY_RvtFormatVersion);
    std::string product = Narrow(GetStr(store, PKEY_RvtProductYear));
    std::string build   = Narrow(GetStr(store, PKEY_RvtBuild));
    std::string locale  = Narrow(GetStr(store, PKEY_RvtLocale));
    std::string ws      = Narrow(GetStr(store, PKEY_RvtWorksharing));

    const wchar_t* fileName = wcsrchr(path, L'\\');
    fileName = fileName ? fileName + 1 : path;

    char nameBuf[512] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, fileName, -1, nameBuf, sizeof(nameBuf) - 1, nullptr, nullptr);

    printf("  %-44s props=%-2lu ver=%-5lu build=%-22s locale=%-5s ws=%s\n",
           nameBuf, (unsigned long)count, (unsigned long)fmt,
           build.empty() ? "-" : build.c_str(),
           locale.empty() ? "-" : locale.c_str(),
           ws.empty() ? "-" : ws.c_str());

    (void)product;
    store->Release();
    return (count > 0 && fmt > 0) ? 0 : 2;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        wprintf(L"用法: rvtvhandlertest.exe <file.rvt | 目录> [...]\n");
        wprintf(L"      目录会递归扫描 .rvt/.rfa/.rte/.rft\n");
        return 1;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetConsoleOutputCP(CP_UTF8);

    HMODULE dll = LoadLibraryW(L"RvtVersionHandler.dll");
    if (!dll)
    {
        printf("[x] 加载 RvtVersionHandler.dll 失败: %lu\n", GetLastError());
        CoUninitialize();
        return 1;
    }

    PFN_DllGetClassObject pfn = (PFN_DllGetClassObject)(void*)GetProcAddress(dll, "DllGetClassObject");
    if (!pfn)
    {
        printf("[x] 找不到 DllGetClassObject\n");
        FreeLibrary(dll);
        CoUninitialize();
        return 1;
    }

    printf("\n=== RvtVersion 属性处理器测试 ===\n\n");

    int failed = 0, total = 0;

    for (int i = 1; i < argc; i++)
    {
        DWORD attr = GetFileAttributesW(argv[i]);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            // 目录：递归扫描，避免中文路径经 cmd 传参被代码页破坏
            std::vector<std::wstring> files;
            ScanDirectory(argv[i], files);
            printf("  [目录] %ls   (%zu 个 Revit 文件)\n",
                   argv[i], files.size());
            for (const auto& f : files)
            {
                total++;
                if (TestFile(pfn, f.c_str()) != 0) failed++;
            }
        }
        else
        {
            total++;
            if (TestFile(pfn, argv[i]) != 0) failed++;
        }
    }

    printf("\n共 %d 个文件，成功 %d，失败 %d\n", total, total - failed, failed);

    FreeLibrary(dll);
    CoUninitialize();
    return failed == 0 ? 0 : 1;
}
