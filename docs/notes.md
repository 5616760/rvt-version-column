# 实现笔记

本文件收录逆向过程、内部实现细节与实测记录。安装使用见 [README](../README.md)。

---

## 一、原版安装包逆向结论

分析对象是本目录的 `setup.exe` + `安装包项目.msi`。

| 分析对象 | 结论 | 证据 |
|---|---|---|
| `setup.exe` (566,272 B) | Visual Studio 生成的 Bootstrapper，只负责解包并启动 MSI | PE 调试路径含 `\Bootstrapper\Engine\setup.pdb`；字符串含 `LaunchApplication` / `InstallSolution` |
| `安装包项目.msi` (548,352 B) | VS Installer Project 产物，x86 / 简体中文 | SummaryInformation：`Intel;2052`；ProductCode `{E97885A4-84F2-4D54-BF16-9567C41769A4}` |
| MSI 内嵌 PE | **不是** Revit 工具本体 | 文件描述 `Microsoft (R) Visual Studio UrlConvert custom action`，版本 `16.0.31603.11111`（即 `DPCA.DLL`，VS CustomActions） |

**判断：该安装包是半成品或占位包，真正的功能 DLL 未随包提供。**
内嵌的 `DPCA.DLL` 已提取到 `analysis/embedded.exe` 留档。

本项目的核心逻辑全部自研。

---

---

## 二、技术原理

### 2.1 版本信息存在哪

`.rvt` / `.rfa` / `.rte` / `.rft` 都是 **OLE2 复合文档**（Compound File Binary）。
版本信息在名为 `BasicFileInfo` 的流里。

### 2.2 CFB 读取：扇区偏移公式

```
扇区起始偏移 = sectorSize × (sectorId + 1)
```

- v3（`sectorShift = 9`）→ 扇区 512 字节，首扇区在 512
- v4（`sectorShift = 12`）→ 头部补齐到 4096，首扇区在 4096

**别硬编码 `512 + id × size`**，那会让所有 4096 扇区文件（大模型）解析失败。

### 2.3 BasicFileInfo 跨版本布局差异（本轮核心难点）

不同 Revit 版本写进 `BasicFileInfo` 的结构**不一样**，而且差异不是「加个字段」那种：

| 版本 | 特征 |
|---|---|
| 2008 | 只有 `Revit Build: 20080126_1900`，**没有** `Format:` 字段 |
| 2016 / 2018 | 偏移 `+14` 处是整句 `Autodesk Revit 2016 (Build: 20150110_1515(x64))` |
| 2020 | 长度前缀串序列 + key-value 纯文本段，且 **UTF-16 奇偶对齐会错位**（shift=1 才读得到 key） |
| 2023 | 第一个串是保存路径，版本串在第二位；`Format: 2023Build: ...` 之间**无分隔符** |

所以**不能用固定偏移**。上一版 C# 实现硬编码了 `+14 / +18 / +26`，
结果是 2023 文件读不到构建号（CLI 输出 `BuildVersion : -`）。

现在的做法是**语义识别 + 双向对齐**：

1. 对 `shift = 0` 和 `shift = 1` 两种 UTF-16 对齐分别做一次全文解析
2. 每个字段（format / build / locale）在两次结果里**择优选取**，而不是整段二选一
3. `Format:` 之后允许无分隔符（兼容 2023 的 `Format: 2023Build:`）
4. 格式号缺失时，用构建号前 4 位反推（兼容 2008）
5. key 匹配**不区分大小写**（真实文件里 `Locale When Saved:` 和 `Locale when saved:` 同时存在）

另外两个坑：

- **长度前缀单位是「字符数」，不是「字节数」**。构建号 18 个字符 → 前缀 `0x12` → 占 `18 × 2 = 36` 字节。
- **构建号收集要从 `YYYYMMDD_HHHH` 精确起算**，只在紧跟 `(` 时做括号配平收集 `(x64)`。
  从「`Build:` 后一直收到行尾」会把外层 `(Build: ...)` 的右括号也吃进来，
  产出 `20170117_1515(x64))º` 这种脏值。

### 2.4 格式版本号 → 产品年份

2016 及之后基本 1:1。更早的版本格式号与商业版本号不重合，
例如格式号 `2012` 对应 Revit 2012（商业号 15.0）。

代码里刻意分两个属性：

- `RvtVersion.ProductYear` → `"12.0 / 2012"`　← 给人看的完整标签
- `RvtVersion.YearShort` → `"2012"`　　　　← 纯年份，便于排序 / 建图标键

**这是必须分开的**：早期把两者混用，图标文件名变成了 `rvtv_1502012.ico`。

---

---

## 三、编译与内部实现

### 3.1 编译

本机 MSVC 14.51 有 `bin`/`lib` 但**没有 `include`**，Windows SDK 也缺 `Include`/`Lib`。
所以走 MinGW-w64。

```bat
set MINGW_ROOT=C:\mingw64
cd src\RevitVersion.Native
build.bat
```

产物（`out\`，安装程序会自动复制一份到项目根目录）：

| 文件 | 大小 | 用途 |
|---|---|---|
| `RvtVersionHandler.dll` | 242,176 B | 属性处理器（x64） |
| `rvtvhandlertest.exe` | 268,800 B | 不注册就能测的验证工具 |
| `rvtvpropschema.exe` | 253,440 B | 注册 / 注销 / 验证 propdesc |
| `RvtVersionSetup.exe` | 776,192 B | 单文件安装程序（内嵌上面三个） |

DLL 只依赖 UCRT（Win10+ 自带），静态链接 libstdc++ / libgcc。

### 3.2 安装程序做了什么

`RvtVersionSetup.exe` 是原生 C++ 程序，不是脚本包装：

1. 停掉 `explorer.exe` 释放 DLL 占用
2. 建 `%ProgramFiles%\RvtVersion`，从自身 RCDATA 释放 3 个载荷
3. 设目录 DACL（`BU` 读执行 / `SY` `BA` 全控），保证普通用户能加载 DLL
4. 写 CLSID + InprocServer32 **默认值**（`RegSetValueEx(name=nullptr)`）
5. **清理旧注册**：`PurgeOldSchemas` 扫 `PropertySchema\*`，删掉指向本 propdesc 的条目，
   再走 `PSUnregisterPropertySchema` 清 URI/CompactURI 缓存
6. `PSRegisterPropertySchema` → `PSRefreshPropertySchema` → 实测 8 个属性可读
7. 挂 4 个扩展名的 `ShellEx\{8895b1c6-...}` + `SystemFileAssociations` 的 `PreviewDetails`/`InfoTip`
8. **清理上一代 overlay 残留**：`ShellIconOverlayIdentifiers` 里的 `RvtVersion` 键、旧 CLSID、
   旧 ProgID、旧 .NET DLL
9. 写 `Uninstall\RvtVersion`（显示名 / 卸载命令 / EstimatedSize），刷新 Shell 关联
10. 重启资源管理器

每一步都做**写后回读校验**，任一项失败立即报错并恢复 explorer，不会「装了一半说成功」。

清单用 `asInvoker` 而非 `requireAdministrator`：非管理员时程序自己 `ShellExecuteEx("runas")`
重新拉起，这样同一个 exe 也能跑 `/extract` 做载荷自检。

### 3.3 显示列

1. 打开含 `.rvt` 的文件夹，切到「详细信息」视图
2. 在任意列头（如「大小」）上右键 → 「选择列」/「其他…」
3. 勾选：

| 列名 | 显示示例 | 说明 |
|---|---|---|
| **Revit Version** | `2023` / `12.0 / 2012` | 推荐 |
| Revit Year | `2023` | 纯年份，便于排序 |
| Revit Format | `2023` | UInt32，可筛选 `>= 2021` |
| Revit Build | `20250724_1515(x64)` | 构建号 |
| Revit Locale | `ENU` / `ENG` | 语言 |
| Revit Worksharing | `Central` / `-` | 工作共享 |

### 3.4 验证

```powershell
powershell -ExecutionPolicy Bypass -File check-install.ps1 -ProbeDir "D:\some\rvt\folder"
```

13 项检查：文件 / CLSID / InprocServer32 默认值 / 4 个扩展名挂载 / schema 注册 /
属性系统实际可读数 / 真实文件解析。
输出 `通过 13 失败 0` 即为完全就绪。

只测解析、不装任何东西：

```powershell
src\RevitVersion.Native\out\rvtvhandlertest.exe "D:\some\rvt\folder"
```

### 3.5 卸载

设置 → 应用 → 「RVT-V · 资源管理器 Revit 版本列」→ 卸载。

或：

```bat
"C:\Program Files\RvtVersion\RvtVersionSetup.exe" /uninstall
```

只删「值等于本工具 CLSID」的键，不误删第三方处理器。
被占用的文件登记到 `MOVEFILE_DELAY_UNTIL_REBOOT`，重启后自动清理。

### 3.6 自动勾选列（`/columns`）

```bat
"C:\Program Files\RvtVersion\RvtVersionSetup.exe" /columns          :: 写入
"C:\Program Files\RvtVersion\RvtVersionSetup.exe" /remove-columns   :: 撤销
```

**当前实现状态：代码已就绪，但在本机被系统 ACL 挡住，需接管所有权才生效。**
详见 8.5。**这个能力已被 6.7 的列监视器取代，不再是主路径。**

`ColumnList` 的真实结构（实测 Win10 22H2，与网上多数资料不同）：

```
HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\FolderTypes\{<FolderTypeGUID>}
    \TopViews\{00000000-0000-0000-0000-000000000000}
        [ColumnList] = REG_SZ
            "prop:0(34)System.ItemNameDisplay;0System.DateModified;1System.Size;..."
```

要点：

- `ColumnList` 是**值**，不是子键。网上流传的「子键名为数字序号」是旧版结构。
- 语法 `可见性(宽度)canonical-name`，`;` 分隔。`0` = 显示，`1` = 隐藏。
- **`prop:` 前缀只在整串开头出现一次**，追加项不能再带，否则整串失效。
- 路径必须带 `KEY_WOW64_64KEY`。

本工具写入的是这两个最通用的 FolderType：

| GUID | CanonicalName | 用途 |
|---|---|---|
| `{5c4f28b5-f869-4e84-8e60-f11db97c5cc7}` | `Generic` | 通用文件夹（绝大多数目录） |
| `{7d49d726-3c21-4f05-99aa-fdc2c9474656}` | `Documents` | 文档类文件夹 |

刻意不碰图片 / 音乐 / 视频等专用类型，避免污染。

### 3.7 按文件夹条件化显示（`rvtvwatch.exe`）★ 主路径

需求原话：「如果打开的文件夹包含 Revit 文件，就自动显示 Revit version 这一列」。

#### 为什么不能用注册表实现

Shell 的列布局只有两个**声明式**来源，两者都无法表达「取决于文件夹内容」：

| 来源 | 粒度 | 能否表达条件 |
|---|---|---|
| `FolderTypes\<type>\TopViews\...\ColumnList` | 按文件夹**类型** | ❌ 对同类型所有文件夹一视同仁 |
| `Bags\<slot>\Shell\<type>\ColInfo` | 按**具体文件夹** | ❌ 是二进制私有格式，只能存「列有哪些」，没有条件位 |

实测验证（`rvtvdiag --view-cols` 真实拉起 Explorer 视图问 Shell 要答案）：

```
写 HKCU 的 Generic ColumnList 后：
  D:\Projects\Tower\RVT          可见列 5 个 ... RvtVersion.ProductYear   ← 含 rvt，正确
  C:\Windows                     可见列 5 个 ... RvtVersion.ProductYear   ← 不含 rvt，多出空列
  C:\Users\<user>\Downloads      可见列 5 个 ... RvtVersion.ProductYear   ← 同上
```

结论：**HKCU 覆盖确实被 Shell 采用**（这是好消息，免提权可写），
但它是**全局**的，无关文件夹也会多出一列空值 —— 不满足需求。

#### 实际方案：常驻监视器 + `IColumnManager::SetColumns`

在该文件夹的视图活着时，直接通过 COM 接口实时增删列。

```
每 700ms:
  枚举 IShellWindows 的所有窗口
    → QueryService(SID_STopLevelBrowser) 拿 IShellBrowser
    → IFolderView2::GetFolder(IID_IPersistFolder2)::GetCurFolder 拿真实路径
    → 探测该目录有没有 .rvt/.rfa/.rte/.rft
    → 有 且 列不可见  → SetColumns(追加)
    → 没有 且 列可见  → SetColumns(移除)
```

#### 为什么 SetColumns 能持久化

这是整个方案能成立的关键，也是**必须实测才能确认**的一点。

`SetColumns` 会把结果写回该文件夹自己的 Bag：

```
rvtvdiag --bag-hashes    # 操作后逐个 Bag 检查 ColInfo 里有没有我们的 FMTID

  1048   Shell  {5C4F28B5-...}  cb=232  hash=6D0B120E65D5B93F  << 含 RvtVersion
  1051   Shell  {5C4F28B5-...}  cb=160  hash=03652CFD43D488D7  << 含 RvtVersion
  1052   Shell  {5C4F28B5-...}  cb=160  hash=14DF9EA3D35E1AE3  << 含 RvtVersion
  1056   Shell  {5C4F28B5-...}  cb=160  hash=85AF38E65C666226  << 含 RvtVersion
  1065   Shell  {7D49D726-...}  cb=160  hash=03652CFD43D488D7  << 含 RvtVersion
```

**但有个致命前提**：只在「真实的资源管理器窗口」上才持久化。

| 操作对象 | 立即生效 | 关掉重开还记得 |
|---|---|---|
| 自己 `CoCreateInstance` 的 `IExplorerBrowser` | ✅ | ❌ 用的是自己的 property bag |
| 真实资源管理器窗口（IShellWindows 枚举到的） | ✅ | ✅ 写进 Bags |

实测证据 (`--set-cols` vs `--real-windows`)：

```
# 自建浏览器：返回 S_OK，但新进程复查仍是 4 列
=== 按文件夹改列: ...\RVT  (add) ===
  SetColumns(追加) hr=0x00000000
  --- 新开视图复查 ---
  可见列 4 个: ...（没有我们的列）
  我们的列: 未可见

# 真实窗口：改完关掉重开，列还在
  含 Revit 文件的窗口: 2
  已改动: 1
→ 关闭该窗口，重新打开同一文件夹
  [不变] D:\Projects\Tower\RVT  含Revit=是 列可见=是
```

#### 接口层的坑

| 坑 | 正确做法 |
|---|---|
| `IDispatch` 直接 QI `IShellBrowser` → `E_NOINTERFACE` | 先 QI `IServiceProvider`，再 `QueryService(SID_STopLevelBrowser, IID_IShellBrowser)` |
| 窗口标题不可靠（可能只显示文件夹名） | `IFolderView2::GetFolder(IID_IPersistFolder2)::GetCurFolder` 拿 PIDL 再 `SHGetPathFromIDListW` |
| 图标/平铺视图下 `GetColumnCount` 返回 0 | 判 `nVis > 0` 才动列，避免误判「列不存在」而反复追加 |
| 无条件每轮都 `SetColumns` | 视图会反复重排、肉眼可见闪烁。必须先比较签名，只在变化时才调 |

#### 安装器里的拉起方式

不能用 `CreateProcessW` 直接从提权安装器起监视器 —— 安装器一退出，
子进程会被一起终止（实测：日志显示「监视器已启动 pid=44612」，但进程立刻消失）。

改用 `ShellExecuteExW`，进程父级落在 explorer 下，与安装器生命周期解耦。
已验证：安装后进程稳定存活，内存约 9.4 MB。

### 3.8 旧的脚本安装方式（保留，已不是主路径）

`install-handler.bat` / `install-handler.ps1` 仍可用，逻辑与 exe 一致。
注意其中的 `Set-RegDefault` ——**写注册表默认值必须用 `Set-Item -Value`**，
用 `Set-ItemProperty -Name '(default)'` 会创建一个名叫 `(default)` 的普通值，
真正的默认值仍为空，导致 DLL 加载不到、列永远空白。这是本项目踩过的最深的坑。

---

---

## 四、验证记录

### 4.1 跨版本解析（真实文件）


| 文件 | 期望 | 实际 |
|---|---|---|
| `Batch Print for Revit 2018\BatchPrintInitial.rvt` | 2008 | ✅ 2008 / `20080126_1900` |
| `Revit 2018\Samples\rac_basic_sample_project.rvt` | 2018 | ✅ 2018 |
| `Revit 2020\Samples\rac_basic_sample_project.rvt` | 2020 | ✅ 2020 / `20190109_1515(x64)` |
| `FormIt\...\2016\Library\Metric\Desk.rfa` | 2016 | ✅ 2016 |
| `RVT 项目\*.rvt`（36 个） | 2023 | ✅ 36/36，`locale` 与 `ws=Central` 全部正确 |

对照：旧 C# CLI 对 2023 文件输出 `BuildVersion : -`，说明固定偏移确实失效。

### 4.2 注册表写入逻辑

`Set-ItemProperty -Name '(default)'` **不会**写注册表默认值，而是创建一个名叫 `(default)` 的
普通值，真正的默认值仍为空 —— CLSID / ShellEx 全部失效，DLL 加载不到。
必须用 `Set-Item -Value`。已改为 `Set-RegDefault` / `Get-RegDefault`，并用 HKCU 复现验证：

```
PASS  CLSID default = RvtVersion Revit Version Property Handler
PASS  InprocServer32 default = C:\Program Files\RvtVersion\RvtVersionHandler.dll
PASS  ProgID default / ProgID\CLSID / CLSID\ProgID
PASS  .rvt/.rfa/.rte/.rft ShellEx = {5AD3E9FC-...}
PASS  no junk (default) value
PASS  ThreadingModel = Both
```

### 4.3 propdesc 根节点必须是 `<schema>`

这是本轮第二个致命坑。最初的 propdesc 直接拿 `<propertyDescriptionList>` 当根：

```xml
<!-- 错：能注册成功，但一条属性都读不出来 -->
<propertyDescriptionList publisher="..." product="..."
    xmlns="http://schemas.microsoft.com/windows/2006/propertydescription">
```

`PSRegisterPropertySchema` **返回 S_OK**，注册表 `PropertySchema\0000` 也写进了正确路径，
但 `IPropertySystem::GetPropertyDescription` 对全部 8 个属性返回 `0x8002802B`
（`TYPE_E_ELEMENTNOTFOUND`），列头菜单里什么都不会出现。

对照系统自带的 `ie9props.propdesc` 才发现正确结构：

```xml
<!-- 对 -->
<schema xmlns="http://schemas.microsoft.com/windows/2006/propertydescription"
        schemaVersion="1.0">
  <propertyDescriptionList publisher="..." product="...">
    <propertyDescription .../>
  </propertyDescriptionList>
</schema>
```

另外 `typeInfo` 要带 `isViewable="true" isQueryable="true"`，否则列也不出现在菜单里。
`searchInfo` 不要写 `columnIndexType="OnDisk"`（那是系统内置属性专用）。

### 4.4 已注册的 schema 不会自动更新

`PSRegisterPropertySchema` 对同一个 propdesc 路径重复调用只返回 `ERROR_ALREADY_EXISTS`，
**属性系统继续用旧的解析结果**。改了 propdesc 内容后必须先把注册表条目彻底删掉。

安装程序的 `PurgeOldSchemas` 就是干这个的：枚举 `PropertySchema\*`，
凡是默认值等于本 propdesc 路径的条目，先 `PSUnregisterPropertySchema` 再 `SHDeleteKey`。

### 4.5 DLL 结构

```
objdump -p out/RvtVersionHandler.dll | grep -A5 "Export Address Table"
→ DllCanUnloadNow / DllGetClassObject   （只有这两个，--kill-at 生效）
→ pei-x86-64
```

安装程序 exe 的资源表（`objdump -x out/RvtVersionSetup.exe`）：

```
RT_MANIFEST   ID 1     0x631
RCDATA        ID 101   0x3b200   242,176 = RvtVersionHandler.dll
RCDATA        ID 102   0x1153      4,435 = RvtVersion.propdesc
RCDATA        ID 103   0x3de00   253,440 = rvtvpropschema.exe
RT_VERSION    ID 16    0x2f0
```

`/extract` 释放出的三个文件 MD5 与源文件逐一比对一致。

### 4.6 schema 工具行为

| 场景 | 结果 |
|---|---|
| `--verify` 未注册 | `已注册 0 / 8`，每项 `hr=0x8002802B`（`TYPE_E_ELEMENTNOTFOUND`） |
| `--register` 非管理员 | `hr=0x80070005`（`E_ACCESSDENIED`），提示需要管理员 |
| `--register` 管理员，根节点错误 | `S_OK`，但 `--verify` 仍 `0 / 8` —— 见 7.3 |
| `--register` 管理员，结构正确 | `S_OK`，`--verify` 返回 `8 / 8`，退出码 0 |

### 4.7 安装全链路（本机实测）

```
RvtVersionSetup.exe /quiet  → exit=0
  [1] 安装文件          3/3 PASS
  [2] COM 类注册        CLSID / InprocServer32 / ThreadingModel PASS
  [3] 文件类型挂载      .rvt .rfa .rte .rft 全部指向本工具 PASS
  [4] schema 已注册 + 属性系统可读 8/8 PASS
  [5] 真实目录解析      36/36 全部返回 8 个属性 PASS
  → 通过 13   失败 0   警告 0
```

### 4.8 条件化列显示（本机实测）

v1.2 的核心能力。三个层面分别验证。

**(a) HKCU FolderTypes 覆盖是否被 Shell 采用** —— 用 `--view-cols` 真实拉起视图：

```
写 HKCU Generic ColumnList 前：
  G:\...\RVT   可见列 4 个   我们的列: 在候选集里但未勾选
写之后：
  G:\...\RVT   可见列 5 个   ... RvtVersion.ProductYear   我们的列: 【可见】
  C:\Windows   可见列 5 个   ... RvtVersion.ProductYear   我们的列: 【可见】   ← 问题
```

结论：被采用，但全局生效 → 不可用于条件化。

**(b) SetColumns 能否持久化** —— 关键分水岭：

| 对象 | SetColumns 返回 | 新进程复查 |
|---|---|---|
| 自建 `IExplorerBrowser` | `S_OK` | ❌ 4 列，改动静默丢失 |
| 真实窗口（IShellWindows） | `S_OK` | ✅ 列还在 |

真实窗口改动后 `--bag-hashes` 显示 5 个 Bag 的 `ColInfo` 里出现我们的 FMTID
（`AE96A02E-F8A8-4063-80CD-E90FDF050B70`），证实写入了该文件夹自己的 Bag。

**(c) 监视器双向条件** —— 实机操作序列：

```
初始：rvclean 目录只有 a.txt
  [不变] ...\RVT      含Revit=是 列可见=是
  [不变] ...\rvclean  含Revit=否 列可见=否

放入 x.rvt（监视器后台自动响应，未手动执行 --once）
  [补上列] ...\rvclean  含Revit=是 列可见=是

删除 x.rvt
  [去掉列] ...\rvclean  含Revit=否 列可见=否

关闭窗口后重新打开 ...\RVT（不做任何操作）
  [不变] ...\RVT  含Revit=是 列可见=是     ← 状态被记住
```

**(d) 安装链路**：

```
[*] 启用按文件夹自动显示列
      开机启动 已注册
      监视器已启动
      规则: 文件夹含 .rvt/.rfa/.rte/.rft -> 自动显示该列
            不含                        -> 自动隐藏该列
```

产物 `rvtvwatch.exe` 236 KB，安装后进程稳定存活（PID 存活 ≥ 4s，内存 9.4 MB）。

---

---

## 五、已知限制与边界

### 5.1 覆盖范围

| 项 | 状态 |
|---|---|
| 列表列显示版本 | ✅ 已实现并安装验证；列头勾选后可见 |
| **含 Revit 文件的文件夹自动显示该列** | ✅ 已实现（`rvtvwatch.exe`），实测双向通过 |
| **不含 Revit 的文件夹不留空列** | ✅ 同上 |
| 2008–2027 跨版本 | ✅ 已用真实文件验证 2008/2016/2018/2020/2023 |
| 排序 / 筛选 | ✅ `Format` 是 UInt32，可按数值筛 |
| 预览窗格 / 悬停提示 | ✅ 已挂 `PreviewDetails` / `InfoTip` |
| 网络路径 | ⚠ 属性处理是**同步**的，慢盘会让列刷新变卡 |
| 加密 / 损坏文件 | ⚠ 只解析不校验，返回空属性 |
| 代码签名 | ❌ 未做，SmartScreen 首次可能提示「未知发布者」 |

### 5.1.1 条件化显示的行为边界

| 场景 | 行为 |
|---|---|
| 文件夹里有 `.rvt/.rfa/.rte/.rft` | 列自动出现 |
| 文件夹里没有 | 列不出现 |
| 窗口开着时新建/删除 `.rvt` | 约 1 秒内自动调整 |
| 关闭窗口再打开 | 状态被记住（写进该文件夹的 Bag），无需重设 |
| 监视器没在运行 | 列维持上次状态，不会自己变 |
| 「详细信息」以外的视图（图标/列表/平铺） | 不动列（没有列概念） |

**只看直接子项，不递归。** 与「详细信息视图里能直接看到该列」的语义一致；
递归会让大目录每次扫描都变慢。

**为什么需要常驻进程**：Windows Shell 没有「文件夹导航到某路径」的公开订阅 API，
未公开的 Shell 事件也不覆盖导航。只能轮询。轮询 700 ms，且只在窗口集合/路径变化时
才真正调 Shell，实测常驻内存约 9.5 MB、空闲 CPU 可忽略。

### 5.2 属性值是缓存的

Windows 属性系统会缓存属性值。文件被 Revit 重新保存后，列里可能仍显示旧版本。
刷新方式：`F5`，或重启资源管理器。

### 5.3 旧的 ColumnHandler 已移除

`RevitVersion.Shell\ColumnHandler.cs`（`IShellFolder2` wrapper 方案）曾保留但**未启用**，
现已连同整个 `RevitVersion.Shell` 项目一并移除 —— `FolderTypes\{GUID}\ColumnHandler`
这条机制微软从未正式文档化，而属性处理器是官方支持的正规路径，已足够覆盖需求。
需要查阅可看 git 历史。

上一代的 overlay 方案（`RevitVersionShell.dll` + `ShellIconOverlayIdentifiers`）
会被安装程序主动清理，避免角标和列同时出现。

### 5.4 32 位进程

DLL 是 x64。64 位资源管理器正常加载。

32 位程序（少数旧工具）若也走 Shell 属性，会加载不到 x64 处理器 —— 这类程序会退化成不显示该列。
如需覆盖，得额外编一份 x86 并注册到 `Wow6432Node`。

### 5.5 自动勾选列受系统 ACL 限制

`/columns` 的代码已实现并验证过逻辑（读取、拼接、写回、回读都正确），
但在本机实测**写入被拒 `rc=5`（ERROR_ACCESS_DENIED），即使提权也不行**。

实测所有者：

```
HKLM\...\Explorer\FolderTypes\{5c4f28b5-...}          所有者 = NT AUTHORITY\SYSTEM
HKLM\...\FolderTypes\{...}\TopViews\{全零GUID}         所有者 = NT AUTHORITY\SYSTEM
当前进程是管理员 = YES（提权后）
```

SYSTEM 拥有的键，Administrators 组不在 DACL 里，管理员也写不进去。
要改必须 `SeTakeOwnership` 接管所有权再改 DACL —— 那会永久改变系统键的
安全属性，属于不可逆操作，**没有放进安装程序自动执行**。

三条可行路径，按侵入性排序：

| 方案 | 侵入性 | 说明 |
|---|---|---|
| 手动勾选一次 | 无 | 列头右键 → 勾「Revit Version」。Shell 会记住到用户 Bag 里，永久生效 |
| 接管所有权后 `/columns` | 中，不可逆 | `takeown` + `icacls` 后执行 `/columns`，系统键所有权永久变更 |
| 构造用户 Bag `ColInfo` | 高，易损 | `ColInfo` 是未公开二进制结构，写错会破坏用户所有文件夹视图 |

**当前默认行为：只注册属性、不强制改列布局。** 装完手动勾一次即可。
如果你确实需要完全免手工，告诉我，我可以加一个显式的 `/take-ownership` 步骤
（带明确警告 + 备份原 ACL），但那是另一个决策。

### 5.6 开机不弹窗：监视器必须链接为 GUI 子系统

**症状**（v1.2.0 及以前）：重启电脑后桌面出现一个 CMD 黑窗口；把它关掉，
再打开资源管理器就看不到 Revit Version 列了。

**根因**：`rvtvwatch.exe` 被编译成控制台子系统。实测 PE 头：

```
IMAGE_OPTIONAL_HEADER.Subsystem = 3   // 3 = WINDOWS_CUI（控制台）
```

`HKCU\...\Run` 启动控制台程序时，系统会为它分配一个控制台窗口 —— 那就是
那个黑窗口。而**那个窗口承载的正是监视器进程本身**，所以关窗 = 结束进程 =
列失效。两个症状是同一个根因，不是两个 bug。

**修复**：链接时加 `-Wl,--subsystem,windows`，`Subsystem` 变 `2`：

```
IMAGE_OPTIONAL_HEADER.Subsystem = 2   // 2 = WINDOWS_GUI（无控制台）
```

GUI 子系统进程不分配控制台，永久无窗口，也就没有可被误关的东西。
`build.bat` 里监视器那一段单独带这个选项，其它产物（安装器、诊断工具）
保持控制台子系统不变 —— 它们需要打印。

**GUI 子系统下的三个实测结论**（MinGW-w64 / g++ 16.2.0 / `-static`）：

| 验证项 | 结果 |
|---|---|
| `wmain(int, wchar_t**)` 是否仍是有效入口 | ✅ 是，不需要改写成 `WinMain` |
| `GetStdHandle(STD_OUTPUT_HANDLE)` 是否有有效句柄 | ✅ 有，`GetFileType` 返回 `FILE_TYPE_PIPE` / `FILE_TYPE_DISK` |
| `WriteFile()` 直写该句柄，管道 / 文件 / 无重定向 | ✅ 三种都正常，中文 UTF-8 无乱码 |

**一个反直觉的坑**：GUI 子系统下不要试图「恢复标准流」。
`_open_osfhandle(handle) + _dup2(fd, 1)` 会以 `errno=9 (EBADF)` 失败，
**并且会破坏本来正常的 stdout** —— 本来能输出的程序，加了这段反而变哑。

所以 `Out()` 的实现方式是 Win32 `WriteFile` 直写句柄，完全绕开 CRT 的 fd 层：

```cpp
static void Out(const wchar_t* fmt, ...)
{
    wchar_t buf[4096];
    // ... _vsnwprintf_s + WideCharToMultiByte(CP_UTF8) ...

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;   // 开机自启无重定向 -> 静默丢弃

    DWORD wrote = 0;
    WriteFile(h, s.data(), (DWORD)s.size(), &wrote, nullptr);
}
```

开机自启时没有重定向，句柄无效，`Out()` 直接返回 —— 不产生任何 UI，
也不写垃圾文件。`--status` / `--once` 从 cmd 或 PowerShell 里跑仍然正常打印。

**同时加的两个约束**：

- `SetConsoleOutputCP` / `SetConsoleCtrlHandler` 改为仅在 `HasConsoleOut()`
  为真时调用。GUI 子系统下它们调用无害但无意义，条件化后语义更清楚。
- 单实例互斥体 `Local\RvtColumnWatcher.SingleInstance`。重复启动（用户误点、
  安装器与 Run 键各起一份）时第二份立即退出，避免多个监视器同时
  `SetColumns` 互相打架（表现为列反复闪烁）。

**怎么自查本机装的是哪一版**：

```powershell
python -c "import struct;d=open(r'C:\Program Files\RvtVersion\rvtvwatch.exe','rb').read();e=struct.unpack('<I',d[0x3c:0x40])[0];print('Subsystem =',struct.unpack('<H',d[e+0x5c:e+0x5e])[0])"
```

`2` = 已修（无窗口），`3` = 旧版（会弹窗）。

**边界**：`Subsystem=2` 是「不弹窗」的充要条件，这一点由 PE 头直接证明，
不需要重启验证。但「重启后仍自动生效」还依赖 `Run` 键存在且路径正确，
那一条由安装器写入，可用 `rvtvwatch.exe --status` 的最后一行确认
（显示「开机启动: 已注册」）。

### 5.7 载荷计数必须单一来源（v1.2.1 修复的安装中止）

**症状**：

```
[3/7] 释放文件
RvtVersionHandler.dll          269312 bytes
RvtVersion.propdesc              5398 bytes
rvtvpropschema.exe             253440 bytes
rvtvdiag.exe                   302592 bytes
rvtvwatch.exe                  236544 bytes
[x] 载荷释放不完整（5/4），安装中止
```

5 个文件全部释放成功，却被判定失败。

**根因**：载荷数量存在两个来源，且互相矛盾。

| 位置 | 值 |
|---|---|
| `kPayloadCount`（1.2.0 加监视器时改过） | `5` |
| `DoInstall()` 里的 `if (n != 4)` 与 `%d/4` | `4`（漏改） |

`ExtractAll()` 返回 5，`5 != 4` 成立 → 中止。讽刺的是 `kPayloadCount` 上方
的注释已经明确警告过「加/减 RCDATA 时必须同步改这里」，但真正做判断的地方
根本没用这个常量。

**修复：消除第二个来源，而不是把 4 改成 5。**

把载荷清单抽成全局唯一表，`kPayloadCount` 从表长度推导：

```cpp
struct Payload { int id; const wchar_t* name; };

static const Payload kPayloads[] = {
    { RES_DLL,        kDllName    },
    { RES_PROPDESC,   kPropDesc   },
    { RES_SCHEMATOOL, kSchemaTool },
    { RES_DIAGTOOL,   kDiagTool   },
    { RES_WATCHER,    kWatcher    },
};

static const int kPayloadCount = (int)_countof(kPayloads);
```

`ExtractAll()` 遍历这张表，`DoInstall()` 和 `/extract` 都用 `kPayloadCount` 校验。
以后加文件只改 `kPayloads` + `Setup.rc`，数量自动跟随，不可能再出现不一致。

**验证**（`/extract` 走的是同一套校验逻辑）：

```
$ RvtVersionSetup.exe /extract <dir>
      RvtVersionHandler.dll          269312 bytes
      RvtVersion.propdesc              5398 bytes
      rvtvpropschema.exe             253440 bytes
      rvtvdiag.exe                   302592 bytes
      rvtvwatch.exe                  236544 bytes
释放 5 / 5
exit code = 0
```

**教训**：写死数字的校验条件，等于给「将来加一个文件」埋一颗定时炸弹；
而且它炸在安装的第一步，看起来像文件释放坏了，实际文件好得很。
校验值必须从唯一数据源推导。

---

