# RVT-V · Revit 文件版本识别工具

在 Windows 资源管理器「详细信息」视图的**列**里直接看到 `.rvt` / `.rfa` / `.rte` / `.rft` 的 Revit 版本。

- 不需要点开属性对话框
- 不需要靠图标角标
- 不需要打开 Revit
- 可以排序、可以筛选（`Revit Format >= 2021`）

```
名称                                   Revit Version   Revit Build            工作共享
T5_MBC_CS-KPF-T5A-ZZ-ARC-M3-00606.rvt  2023            20250724_1515(x64)     Central
rac_basic_sample_project.rvt           2020            20190109_1515(x64)     -
BatchPrintInitial.rvt                  2008            20080126_1900          -
```

---

## 一、安装（一条命令）

**双击 `RvtVersionSetup.exe`**，UAC 点「是」，装完即可用。

单文件 1.3 MB，DLL / propdesc / 注册工具 / 列监视器全部内嵌，不需要解压、不需要 .NET、不需要联网。

装完直接打开任意含 `.rvt` 的文件夹 → 切到「详细信息」→ **列会自动出现，不用勾选**。
不含 Revit 文件的文件夹不会多出空列。

卸载：设置 → 应用 → 找「RVT-V · 资源管理器 Revit 版本列」→ 卸载。
或直接跑 `C:\Program Files\RvtVersion\RvtVersionSetup.exe /uninstall`。

安装程序命令行：

| 参数 | 作用 |
|---|---|
| （无） | 正常安装，自动提权 |
| `/uninstall` | 卸载 |
| `/quiet` | 静默，不等按键 |
| `/extract <dir>` | 只释放载荷，不注册（自检用，不需要管理员） |

日志落在 `%TEMP%\RvtVersionSetup.log`。

### 1.1 列监视器（`rvtvwatch.exe`）

安装时自动注册开机启动（`HKCU\...\Run`）并立即拉起，用户无需操作。

```powershell
rvtvwatch.exe --status   # 看当前各窗口状态（含 Revit / 列是否可见）
rvtvwatch.exe --once     # 只同步一次就退出
rvtvwatch.exe --install  # 注册开机启动
rvtvwatch.exe --remove   # 取消开机启动
taskkill /IM rvtvwatch.exe /F   # 临时停掉
```

轮询 700 ms，但**只在窗口集合或路径变化时才真正动 Shell**，空闲时 CPU 占用可忽略
（实测常驻内存约 9.5 MB）。

---

## 二、为什么之前「列表里看不到」

上一版只做了 `ShellIconOverlayIdentifiers`（图标覆盖层）。覆盖层是在图标角落叠小角标，
和**列表列**是两套完全独立的 Shell 机制，所以列表里自然什么都没有。

要在列表里显示自定义字段，Windows 只有一条正规路径，三个组件缺一不可：

| 组件 | 作用 | 缺了会怎样 |
|---|---|---|
| **Property Handler**（COM 原生 DLL） | 按需解析文件，返回属性值 | 列是空白 |
| **Property Description**（`.propdesc` XML） | 向属性系统声明「存在这个字段」 | 列头右键菜单里**找不到**这个列 |
| **ShellEx 注册** | 把处理器挂到 `.rvt` 等扩展名上 | 资源管理器不会去调你的 DLL |

三样都注册到 HKLM，所以需要管理员权限。

另外还有两个容易漏的注册位置（本轮踩到过，见排障章节）：

| 位置 | 作用 |
|---|---|
| `HKCR\.rvt\ShellEx\{8895b1c6-...}` | 旧式 handler 挂载点 |
| `HKLM\...\PropertySystem\PropertyHandlers\.rvt` | **Win7+ Shell 的主查找路径** |

只写前者 → 列头能出现，但**每行全空白**。这个症状组合极具误导性。

---

---

## 三、工程结构

```
src/
  RevitVersion.Native/              ★ 属性处理器 + 安装程序（C++，交付主路径）
    PropKeys.h                      自定义 FMTID + 8 个 PROPERTYKEY
    RevitCfb.h                      原生 OLE2 只读读取器（FAT/MiniFAT/DIFAT）
    BasicFileInfo.h                 跨版本语义解析（双向对齐 + 字段择优）
    PropertyHandler.cpp             IPropertyStore / IInitializeWithStream|File
                                    / IPropertyStoreCapabilities / IPropertyStoreFactory
    ShellColumnSync.h               ★ 按文件夹条件化列的核心逻辑（可复用）
    RvtColumnWatcher.cpp            ★ 常驻监视器 rvtvwatch.exe
    ShellDiag.cpp                   诊断工具 rvtvdiag.exe（注册检查 / 真实视图取证）
    Setup.cpp                       安装程序主体（提权 / 注册 / 卸载 / 自检）
    Setup.rc + Setup.manifest       载荷嵌入 + 应用清单
    PropSchemaTool.cpp              propdesc 注册 / 注销 / 验证（替代 Add-Type）
    HandlerTest.cpp                 进程内加载 DLL、枚举 8 个属性（不需注册即可测）
    RvtVersion.propdesc             属性描述 schema
    RvtVersionHandler.def           只导出 DllGetClassObject / DllCanUnloadNow
    build.bat                       MinGW 构建（一次产出全部五个 exe/dll）

  RevitVersion.Core/                netstandard2.0，零第三方依赖（CLI 复用）
    CompoundFileReader.cs           自研最小 OLE2 只读读取器
    RevitFileProbe.cs               探针（注意：仍是旧固定偏移实现）
    RevitVersionMap.cs              格式号 → 年份映射

  RevitVersion.Cli/                 net10.0-windows，命令行工具 rvtv.exe
```

**交付主路径是 `RevitVersion.Native`**。`RevitVersion.Core` / `Cli` 是早期探索产物，
保留备查；CLI 的固定偏移实现对 2023 文件仍然不准，以原生 DLL 为准。

早期还有过一个 `RevitVersion.Shell`（图标覆盖层 / `IShellFolder2` wrapper 两代方案），
已被属性处理器 + 列监视器完全取代，代码已移除（见 git 历史）。

`rvtvdiag.exe` 是排障主力，子命令一览：

| 子命令 | 用途 |
|---|---|
| `--check` | 检查全部注册项（CLSID / ShellEx / PropertyHandlers / schema） |
| `--store <file>` | 走 Shell 的真实代码路径读属性并打印 |
| `--view-cols <dir...>` | **真实拉起 Explorer 视图**，问 Shell「这个文件夹显示哪些列」 |
| `--view-cols --persist <dir>` | 同上，但会读取已保存的视图状态（验证持久化） |
| `--set-cols <dir> add\|remove` | 用自建浏览器改列（**不持久化**，仅用于对照实验） |
| `--real-windows [add\|remove\|force-add\|force-remove]` | 枚举/操作真实窗口的列 |
| `--close-windows <子串>` | 正常关闭匹配窗口（触发视图状态保存） |
| `--bag-hashes` | 逐个 Bag 检查 `ColInfo` 哈希与是否含我们的 FMTID（持久化硬证据） |
| `--list-foldertypes` / `--dump-cols` | 列出 FolderType 与 ColumnList |
| `--add-col-user` / `--del-col-user <GUID>` | 往 HKCU 写/撤 FolderTypes 覆盖 |
| `--access <GUID>` | 检查某键的读写权限与所有者 |
| `--probe-user-paths` | 探测 HKCU 各候选路径可写性 |
| `--user-bags` / `--rvt-bags` / `--bagmru-raw` | 查看用户视图缓存 |

---

---

## 四、构建

需要 MinGW-w64（g++）。设 `MINGW_ROOT` 指向其根目录，或把 `g++` 放进 PATH。

```bat
cd src\RevitVersion.Native
set MINGW_ROOT=C://mingw64
build.bat
```

产物在 `out\`：`RvtVersionHandler.dll`、`rvtvwatch.exe`、`rvtvdiag.exe`、`rvtvpropschema.exe`、`RvtVersionSetup.exe`。

.NET 部分（`RevitVersion.Core` / `RevitVersion.Cli`）单独构建：

```bat
dotnet build src\RevitVersion.Core -c Release
dotnet build src\RevitVersion.Cli  -c Release
```

编译细节、实现原理、逆向过程、实测数据见 **[docs/notes.md](docs/notes.md)**。

---

## 五、排障

**先跑这一条**，13 项检查会直接告诉你哪一环断了：

```powershell
powershell -ExecutionPolicy Bypass -File check-install.ps1 -ProbeDir "D:\some\rvt\folder"
```

### 列头菜单里找不到「Revit Version」

两种可能，按顺序查：

```powershell
# A. 注册表里有没有条目指向我们的 propdesc
$b='HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertySchema'
Get-ChildItem $b | ForEach-Object {
  (Get-ItemProperty $_.PSPath).'(default)'
}

# B. 属性系统能否真读到（这个才是关键）
& 'C:\Program Files\RvtVersion\rvtvpropschema.exe' --verify
```

- A 有路径、B 是 `0 / 8` → **propdesc 结构错了**。根节点必须是 `<schema schemaVersion="1.0">`，
  见 7.3。改完必须彻底删掉 `PropertySchema\*` 里那一条再重新注册（见 7.4）。
- A 没有路径 → `PSRegisterPropertySchema` 失败，看 `%TEMP%\RvtVersionSetup.log`。

### 列出现了，但全是空白

**这是本项目踩得最深的坑，根因是 `PropertySystem\PropertyHandlers` 未注册。**

一条命令定位（自带工具，不需要管理员）：

```powershell
& 'C:\Program Files\RvtVersion\rvtvdiag.exe' --check
```
看输出的 `汇总: ShellEx N/4, PropertyHandlers M/4`：

| 现象 | 含义 | 处理 |
|---|---|---|
| `PropertyHandlers 0/4` | **直接原因**：Shell 取 `IPropertyStore` 的主查找路径没注册 | 重装（1.1.0 起已自动写） |
| 两行都 4/4，但列仍空 | 属性缓存 | 重启资源管理器 |
| `PropHandlers 4/4` 且 `--store` 有值 | 已正常，只是没勾列 | 列头右键勾选，或 `--columns` |

再确认 Shell 真的能取到值（与资源管理器完全相同的代码路径）：

```powershell
& 'C:\Program Files\RvtVersion\rvtvdiag.exe' --store "D:\path\to\file.rvt"
```

期望输出末尾是：

```
flags=0x01 HANDLERPROPERTIESONLY    hr=0x00000000
[HANDLERPROPERTIESONLY] 属性总数 = 8
    >> ProductYear = "2023"
结论: Shell 能拿到 Revit Version 值 —— handler 已生效
```

`HANDLERPROPERTIESONLY` 返回 `hr=0x80004005`（E_FAIL）或属性数为 0，
说明 Shell 认为 `.rvt` 没有 handler —— 就是 `PropertyHandlers` 的问题。

#### 为什么 ShellEx 有了还不够

Windows 有**两套并行的注册位置**，缺一不可：

| 位置 | 作用 |
|---|---|
| `HKCR\.rvt\ShellEx\{8895b1c6-b41f-4c1c-a562-0d564250836f}` | 旧式接口，部分组件仍查 |
| `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers\.rvt` | **Win7+ Shell 主查找路径** |

只写前者：schema 已注册 → 列头能出现；但 handler 不被调用 → **每一行都空白**。
这个组合症状极具误导性，看起来像"解析失败"，实际是"根本没被调用"。

必须用 `KEY_WOW64_64KEY` 写，64 位 Shell 读不到 32 位视图。

#### 如果两处都注册了还是空白

按顺序排查：

```powershell
# 1. InprocServer32 默认值是否真指向 DLL（不能为空，也不能有垃圾 (default) 值）
$k=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\Classes\CLSID\{5AD3E9FC-A720-47F6-8DFA-9AACD3286E01}\InprocServer32')
"default = [" + $k.GetValue('') + "]"
"names   = " + ($k.GetValueNames() -join ' | ')

# 2. 绕过 Shell，直接测解析
& 'C:\Program Files\RvtVersion\RvtVersionSetup.exe' /extract "$env:TEMP\rvtvchk" | Out-Null
& "$env:TEMP\rvtvchk\rvtvhandlertest.exe" "D:\some\folder"
```

- 1 的 `default` 为空，或 `names` 里出现 `(default)` 这个**值名**
  → 用了 `Set-ItemProperty -Name '(default)'` 写默认值（那是错的，会创建普通值）。
  重装即可修正。
- 2 有值、1 正常、`--check` 两行都 4/4，列仍空白 → 重启资源管理器（属性缓存）。
- 2 也全是 `props=0` → 解析层问题，看 7.3 / 7.4。


### 安装程序报「提权失败或被取消」

UAC 被拒绝。右键 `RvtVersionSetup.exe` → 「以管理员身份运行」。

### 列没有自动出现 / 不该出现时却出现

先看监视器在不在跑：

```powershell
tasklist /FI "IMAGENAME eq rvtvwatch.exe"
& 'C:\Program Files\RvtVersion\rvtvwatch.exe' --status
```

`--status` 的输出会直接告诉你每个窗口的判断结果：

```
=== 当前窗口状态 ===
  窗口 2 个（文件系统视图）
    [不变  ] D:\Projects\Tower\RVT         含Revit=是 列可见=是
    [不变  ] C:\Users\...\Temp\rvclean      含Revit=否 列可见=否
```

| 现象 | 原因 | 处理 |
|---|---|---|
| 进程不在 | 被手动 kill，或开机自启被关 | `rvtvwatch.exe --install`，再手动起一次 |
| `含Revit=否` 但目录里明明有 `.rvt` | **只看直接子项，不递归**；子文件夹里的不算 | 设计如此，见 [notes 5.1.1](docs/notes.md#511-条件化显示的行为边界) |
| `含Revit=是 列可见=否` 反复出现 | 该窗口不是「详细信息」视图（图标/列表/平铺没有列概念） | 切到详细信息 |
| 列出现但没值 | 不是监视器的问题，是 handler 没生效 | 回到上一节 `--check` |
| `--status` 显示「窗口 0 个」 | 没开任何文件夹窗口（桌面不算） | 正常 |

手动验证一次（不改状态，只打印）：

```powershell
& 'C:\Program Files\RvtVersion\rvtvwatch.exe' --once
```

**为什么必须常驻**：Shell 没有「导航到某文件夹」的公开订阅 API。
所以是 700 ms 轮询，但只在窗口集合/路径变化时才真正调 Shell，
不影响日常使用（内存约 9.5 MB）。

### 卸载后列还留着

卸载会 `taskkill` 监视器并清 Run 键，但**已经被写进各文件夹 Bag 的列状态不会自动清**。
这是 Shell 的持久化设计，不是残留 bug。彻底清：

```powershell
# 关掉所有资源管理器窗口后
Get-Process explorer | Stop-Process -Force
```

或者手动在列头上把该列取消勾选一次。

### 旧版图标角标还在

上一代 overlay 的注册项未被清除。安装程序会主动清，但需要重启资源管理器才生效：

```powershell
Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers' |
  Where-Object { $_.PSChildName -like '*RvtVersion*' }
```

期望无输出。

### 悬停提示不显示

`InfoTip` 已写入 `HKLM\SOFTWARE\Classes\SystemFileAssociations\.rvt`，
但 Windows 只在部分视图显示，且可能被组策略关闭。

### 日志位置

```
%TEMP%\RvtVersionSetup.log
```

安装每一步都有记录，含 `PSRegisterPropertySchema` 的原始 HRESULT。


### 悬停提示不显示

`InfoTip` 已写入 `HKLM\SOFTWARE\Classes\SystemFileAssociations\.rvt`，
但 Windows 只在「详细信息」以外的某些视图显示。若被系统策略关闭则不生效。

---

---

## 六、环境

- Windows 10/11 x64
- 构建：MinGW-w64 16.2（g++ / windres）
- 运行：只依赖 UCRT（系统自带）
- 旧的 .NET 部分：.NET SDK 10.0.402 / .NET Framework 4.8

---

## 相关文档

- [docs/notes.md](docs/notes.md) — 逆向过程、实现细节、验证记录、已知限制

## 许可

MIT
