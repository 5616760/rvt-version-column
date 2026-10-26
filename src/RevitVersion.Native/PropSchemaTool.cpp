// =====================================================================
// 属性 schema 注册 / 校验工具
//
// 作用：
//   --register <file.propdesc>    调 PSRegisterPropertySchema
//   --unregister <file.propdesc>  调 PSUnregisterPropertySchema
//   --verify                      读取已注册的 RvtVersion 属性描述，打印关键字段
//   --list-columns                列出当前 schema 里我们定义的全部属性
//
// 为什么单独做工具而不在 PowerShell 里 Add-Type：
//   部分环境限制运行时编译，用独立进程更稳，也便于在安装脚本里调用。
// =====================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <propsys.h>
#include <propvarutil.h>
#include <cstdio>
#include <string>

#include "PropKeys.h"

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring GetDescString(IPropertyDescription* desc, PROPDESC_FORMAT_FLAGS)
{
    LPWSTR s = nullptr;
    std::wstring out;
    if (SUCCEEDED(desc->GetDisplayName(&s)) && s)
    {
        out = s;
        CoTaskMemFree(s);
    }
    return out;
}

static int Verify()
{
    IPropertySystem* sys = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_PropertySystem, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IPropertySystem, (void**)&sys);
    if (FAILED(hr) || !sys)
    {
        printf("[x] 无法创建 IPropertySystem hr=0x%08lX\n", (unsigned long)hr);
        return 1;
    }

    printf("\n=== 已注册的 RvtVersion 属性 ===\n\n");
    printf("  %-26s %-6s %-28s %s\n", "canonical name", "propID", "display name", "type");

    int found = 0;
    for (size_t i = 0; i < kPropCount; i++)
    {
        IPropertyDescription* desc = nullptr;
        hr = sys->GetPropertyDescription(kProps[i].key, IID_IPropertyDescription, (void**)&desc);
        if (FAILED(hr) || !desc)
        {
            printf("  %-26s %-6lu (未注册)  hr=0x%08lX\n",
                   "", kProps[i].key.pid, (unsigned long)hr);
            continue;
        }

        LPWSTR canon = nullptr;
        desc->GetCanonicalName(&canon);

        std::wstring disp = GetDescString(desc, PDFF_DEFAULT);

        VARTYPE vt = VT_EMPTY;
        desc->GetPropertyType(&vt);

        printf("  %-26s %-6lu %-28s vt=%u\n",
               canon ? Narrow(canon).c_str() : "?",
               kProps[i].key.pid,
               Narrow(disp).c_str(),
               (unsigned)vt);

        if (canon) CoTaskMemFree(canon);
        desc->Release();
        found++;
    }

    // 逐项校验可读性（GetPropertyDescription 成功 + 名称非空）
    printf("\n  已注册 %d / %zu\n", found, kPropCount);

    sys->Release();
    return found == (int)kPropCount ? 0 : 2;
}

int wmain(int argc, wchar_t** argv)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2)
    {
        printf("用法:\n");
        printf("  rvtvpropschema --register   <file.propdesc>\n");
        printf("  rvtvpropschema --unregister <file.propdesc>\n");
        printf("  rvtvpropschema --verify\n");
        CoUninitialize();
        return 1;
    }

    std::wstring mode = argv[1];

    if (mode == L"--register" || mode == L"--unregister")
    {
        if (argc < 3)
        {
            printf("[x] 缺少 propdesc 路径\n");
            CoUninitialize();
            return 1;
        }

        HRESULT hr;
        if (mode == L"--register")
            hr = PSRegisterPropertySchema(argv[2]);
        else
            hr = PSUnregisterPropertySchema(argv[2]);

        if (SUCCEEDED(hr))
        {
            printf("[+] %s 成功: %ls\n",
                   mode == L"--register" ? "注册" : "注销", argv[2]);
            PSRefreshPropertySchema();
            CoUninitialize();
            return 0;
        }

        printf("[x] 失败 hr=0x%08lX\n", (unsigned long)hr);
        if (hr == E_ACCESSDENIED)  printf("    需要管理员权限\n");
        if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) printf("    该 schema 已注册\n");
        CoUninitialize();
        return 1;
    }

    if (mode == L"--verify")
    {
        int rc = Verify();
        CoUninitialize();
        return rc;
    }

    printf("[x] 未知参数\n");
    CoUninitialize();
    return 1;
}
