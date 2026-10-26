// =====================================================================
// 自定义属性键定义
//
// 使用独立 FMTID，避免与系统属性（System.*）冲突。
// 每个属性都必须同时在 RvtVersion.propdesc 中声明，
// 否则资源管理器不会在「详细信息」里提供该列。
// propID 从 2 开始（0 和 1 被系统保留）。
// =====================================================================
#pragma once

#include <propkeydef.h>
#include <propsys.h>

// {AE96A02E-F8A8-4063-80CD-E90FDF050B70}
static const GUID FMTID_RvtVersion =
    { 0xae96a02e, 0xf8a8, 0x4063, { 0x80, 0xcd, 0xe9, 0x0f, 0xdf, 0x05, 0x0b, 0x70 } };

#define PID_RVT_FORMAT_VERSION   2   // 格式版本号（UInt32，可排序）
#define PID_RVT_PRODUCT_YEAR     3   // 展示标签（"12.0 / 2012" 或 "2023"）
#define PID_RVT_YEAR_SHORT       4   // 纯年份（"2023"）
#define PID_RVT_BUILD            5   // 构建号
#define PID_RVT_LOCALE           6   // 语言
#define PID_RVT_WORKSHARING      7   // 工作共享状态
#define PID_RVT_SAVED_PATH       8   // 上次保存路径
#define PID_RVT_SIZE             9   // 文件大小（字节）

#define PKEY_RvtFormatVersion  { FMTID_RvtVersion, PID_RVT_FORMAT_VERSION }
#define PKEY_RvtProductYear    { FMTID_RvtVersion, PID_RVT_PRODUCT_YEAR }
#define PKEY_RvtYearShort      { FMTID_RvtVersion, PID_RVT_YEAR_SHORT }
#define PKEY_RvtBuild          { FMTID_RvtVersion, PID_RVT_BUILD }
#define PKEY_RvtLocale         { FMTID_RvtVersion, PID_RVT_LOCALE }
#define PKEY_RvtWorksharing    { FMTID_RvtVersion, PID_RVT_WORKSHARING }
#define PKEY_RvtSavedPath      { FMTID_RvtVersion, PID_RVT_SAVED_PATH }
#define PKEY_RvtSize           { FMTID_RvtVersion, PID_RVT_SIZE }

struct PropDef
{
    PROPERTYKEY key;
};

// 顺序必须与 PropertyHandler.cpp 中 FillValue 的 case 索引一致
static const PropDef kProps[] =
{
    { PKEY_RvtFormatVersion },
    { PKEY_RvtProductYear   },
    { PKEY_RvtYearShort     },
    { PKEY_RvtBuild         },
    { PKEY_RvtLocale        },
    { PKEY_RvtWorksharing   },
    { PKEY_RvtSavedPath     },
    { PKEY_RvtSize          },
};

static const size_t kPropCount = _countof(kProps);
