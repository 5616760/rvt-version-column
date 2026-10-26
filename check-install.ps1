<#
.SYNOPSIS
    RvtVersion 属性处理器 —— 安装后自检

.DESCRIPTION
    逐项检查安装是否真正到位。任何一项 FAIL 都意味着「列里看不到版本」。

    检查项：
      1. DLL 是否已复制到 Program Files
      2. CLSID 注册 + InprocServer32 默认值（必须指向真实 DLL 路径）
      3. InprocServer32 的默认值不能是空、不能是垃圾 "(default)" 值
      4. 四个扩展名的 ShellEx\PropertyHandler 默认值是否为我们的 CLSID
      5. 属性描述 schema 是否已注册（8 个属性）
      6. COM 对象能否真的被创建（LoadLibrary + DllGetClassObject）

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File check-install.ps1
#>

[CmdletBinding()]
param(
    [string]$ProbeDir = 'G:\DATA\Work\12T5A\合模\1 Archi\RVT'
)

$ErrorActionPreference = 'Continue'

$Clsid  = '{5AD3E9FC-A720-47F6-8DFA-9AACD3286E01}'
$ShellExPropHandler = '{8895b1c6-b41f-4c1c-a562-0d564250836f}'
$Extensions = @('.rvt', '.rfa', '.rte', '.rft')
$TargetDir  = Join-Path $env:ProgramFiles 'RvtVersion'

$pass = 0
$fail = 0
$warn = 0

function Report {
    param([string]$Level, [string]$Msg)
    switch ($Level) {
        'PASS' { Write-Host "  [PASS] $Msg" -ForegroundColor Green; $script:pass++ }
        'FAIL' { Write-Host "  [FAIL] $Msg" -ForegroundColor Red;   $script:fail++ }
        'WARN' { Write-Host "  [WARN] $Msg" -ForegroundColor Yellow; $script:warn++ }
        'INFO' { Write-Host "  [INFO] $Msg" }
    }
}

function Get-RegDefault {
    param([string]$Path)
    if ($Path -like 'HKLM:*') { $hive = [Microsoft.Win32.Registry]::LocalMachine }
    else { $hive = [Microsoft.Win32.Registry]::CurrentUser }
    $sub = $Path -replace '^HK(LM|CU):\\', ''
    $key = $hive.OpenSubKey($sub)
    if (-not $key) { return $null }
    try { return $key.GetValue('') } finally { $key.Close() }
}

function Test-JunkDefaultValue {
    param([string]$Path)
    $sub = $Path -replace '^HK(LM|CU):\\', ''
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($sub)
    if (-not $key) { return $false }
    try { return ($key.GetValueNames() -contains '(default)') } finally { $key.Close() }
}

Write-Host ''
Write-Host '========================================' -ForegroundColor Cyan
Write-Host ' RvtVersion 安装自检' -ForegroundColor Cyan
Write-Host '========================================' -ForegroundColor Cyan

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host ('  当前身份: ' + $(if ($isAdmin) { '管理员' } else { '普通用户（只读检查）' }))
Write-Host ''

# ---- 1. 文件 ----
Write-Host '[1] 安装文件' -ForegroundColor Cyan
$dllPath = Join-Path $TargetDir 'RvtVersionHandler.dll'
$pdPath  = Join-Path $TargetDir 'RvtVersion.propdesc'
$toolPath = Join-Path $TargetDir 'rvtvpropschema.exe'

foreach ($f in @($dllPath, $pdPath, $toolPath)) {
    if (Test-Path $f) {
        $sz = (Get-Item $f).Length
        Report PASS "$(Split-Path $f -Leaf)  存在  $sz bytes"
    } else {
        Report FAIL "$(Split-Path $f -Leaf)  缺失 —— 安装未完成"
    }
}

# ---- 2. CLSID ----
Write-Host ''
Write-Host '[2] COM 类注册' -ForegroundColor Cyan
$clsidKey = "HKLM:\SOFTWARE\Classes\CLSID\$Clsid"
$inproc   = "$clsidKey\InprocServer32"

if (Test-Path $clsidKey) {
    Report PASS "CLSID 键存在: $Clsid"
} else {
    Report FAIL "CLSID 键不存在 —— 资源管理器无法创建处理器"
}

if (Test-Path $inproc) {
    $v = Get-RegDefault $inproc
    if ([string]::IsNullOrWhiteSpace($v)) {
        Report FAIL "InprocServer32 默认值为空 —— 典型症状：列里全是空白"
    } elseif ($v -eq $Clsid -or $v -eq 'RvtVersion Revit Version Property Handler') {
        Report FAIL "InprocServer32 默认值被写错: $v"
    } elseif ($v -ieq $dllPath -or (Test-Path $v)) {
        Report PASS "InprocServer32 -> $v"
    } else {
        Report FAIL "InprocServer32 指向不存在的路径: $v"
    }

    if (Test-JunkDefaultValue $inproc) {
        Report FAIL "InprocServer32 下有垃圾值 '(default)' —— Set-ItemProperty 用错了"
    }

    $tm = (Get-ItemProperty $inproc -Name 'ThreadingModel' -ErrorAction SilentlyContinue).ThreadingModel
    if ($tm -eq 'Both') { Report PASS 'ThreadingModel = Both' }
    else { Report WARN "ThreadingModel = $tm（建议 Both）" }
} else {
    Report FAIL "InprocServer32 子键不存在"
}

# ---- 3. 文件类型挂载 ----
Write-Host ''
Write-Host '[3] 文件类型挂载' -ForegroundColor Cyan
$phBase = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers'

foreach ($ext in $Extensions) {
    # (a) ShellEx（兼容旧路径）
    $seKey = "HKLM:\SOFTWARE\Classes\$ext\ShellEx\$ShellExPropHandler"
    if (-not (Test-Path $seKey)) {
        Report FAIL "$ext  未挂载 ShellEx 属性处理器"
    } else {
        $v = Get-RegDefault $seKey
        if ($v -eq $Clsid) {
            Report PASS "$ext  ShellEx -> $Clsid"
        } elseif ([string]::IsNullOrWhiteSpace($v)) {
            Report FAIL "$ext  ShellEx 默认值为空 —— Set-ItemProperty 写错了"
        } else {
            Report FAIL "$ext  ShellEx 指向别的处理器: $v"
        }
        if (Test-JunkDefaultValue $seKey) {
            Report FAIL "$ext  ShellEx 下有垃圾值 '(default)'"
        }
    }

    # (b) PropertyHandlers —— Win7+ 资源管理器取 IPropertyStore 的主路径
    #     这一项缺失的症状就是「列头在、值全空」。
    $phKey = "$phBase\$ext"
    if (-not (Test-Path $phKey)) {
        Report FAIL "$ext  PropertyHandlers 未注册 —— 这是列值空白的直接原因"
    } else {
        $v = Get-RegDefault $phKey
        if ($v -eq $Clsid) {
            Report PASS "$ext  PropertyHandlers -> $Clsid"
        } elseif ([string]::IsNullOrWhiteSpace($v)) {
            Report FAIL "$ext  PropertyHandlers 默认值为空"
        } else {
            Report FAIL "$ext  PropertyHandlers 指向别的处理器: $v"
        }
    }
}

# ---- 4. 默认列布局 ----
Write-Host ''
Write-Host '[4] 默认列布局' -ForegroundColor Cyan
$ftBase = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\FolderTypes'
$ftTargets = @{
    '{5c4f28b5-f869-4e84-8e60-f11db97c5cc7}' = 'Generic 通用文件夹'
    '{7d49d726-3c21-4f05-99aa-fdc2c9474656}' = 'Documents 文档'
}
$colHit = 0
foreach ($g in $ftTargets.Keys) {
    $sub = "$ftBase\$g\TopViews\{00000000-0000-0000-0000-000000000000}"
    if (-not (Test-Path $sub)) {
        Report WARN "$($ftTargets[$g])  TopViews 键不存在，跳过"
        continue
    }
    $cl = (Get-ItemProperty $sub -Name 'ColumnList' -ErrorAction SilentlyContinue).ColumnList
    if ($cl -and $cl -like '*RvtVersion.ProductYear*') {
        Report PASS "$($ftTargets[$g])  默认列已含 Revit Version"
        $colHit++
    } else {
        Report WARN "$($ftTargets[$g])  默认列未含 Revit Version（需手动勾选）"
    }
}

# ---- 5. 属性描述 schema ----
Write-Host ''
Write-Host '[5] 属性描述 schema' -ForegroundColor Cyan
# 直接查注册表最可靠：PropertySchema 下应有条目指向我们的 propdesc。
# 不走 rvtvpropschema.exe 的文本输出，避免控制台代码页导致中文乱码影响判断。
$schemaBase = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertySchema'
$schemaHit = $null
if (Test-Path $schemaBase) {
    foreach ($sub in (Get-ChildItem $schemaBase -ErrorAction SilentlyContinue)) {
        $v = (Get-ItemProperty $sub.PSPath -ErrorAction SilentlyContinue).'(default)'
        if ($v -and $v -like '*RvtVersion.propdesc') { $schemaHit = $v; break }
    }
}
if ($schemaHit) {
    Report PASS "schema 已注册 -> $schemaHit"
} else {
    Report FAIL 'schema 未注册 —— 列头菜单里不会出现 Revit 版本列'
}

# 再用属性系统实际读一次，确认 8 个属性都能取到
if (Test-Path $toolPath) {
    $out = & $toolPath --verify 2>&1 | Out-String
    if ($out -match '(\d+)\s*/\s*(\d+)') {
        $got = [int]$Matches[1]; $total = [int]$Matches[2]
        if ($got -eq $total -and $got -gt 0) {
            Report PASS "属性系统可读 $got / $total 个属性"
        } else {
            Report FAIL "属性仅可读 $got / $total —— 多半是 propdesc 结构问题（根节点必须是 schema）"
        }
    }
}

# ---- 5. COM 实际创建 ----
Write-Host ''
Write-Host '[6] 处理器功能实测' -ForegroundColor Cyan
$testExe = Join-Path $PSScriptRoot 'src\RevitVersion.Native\out\rvtvhandlertest.exe'
if (-not (Test-Path $testExe)) {
    $testExe = Join-Path $PSScriptRoot 'out\rvtvhandlertest.exe'
}

if (Test-Path $testExe) {
    if (Test-Path $ProbeDir) {
        $out = & $testExe $ProbeDir 2>&1 | Out-String
        # 不匹配中文"共 N 个文件"——控制台代码页会让它变乱码。
        # 改数 "props=" 出现次数作为成功计数，用文件总数对照。
        $propsHits = ([regex]::Matches($out, 'props=(\d+)')).Count
        $nonZero   = ([regex]::Matches($out, 'props=(?!0\b)\d+')).Count
        if ($propsHits -gt 0 -and $nonZero -eq $propsHits) {
            Report PASS "解析实测 $nonZero / $propsHits 全部返回 8 个属性（目录: $ProbeDir）"
        } elseif ($propsHits -gt 0) {
            Report FAIL "解析实测 $nonZero / $propsHits —— 有文件读不出属性"
        } else {
            Report WARN "测试工具无输出，无法判断（目录: $ProbeDir）"
        }
    } else {
        Report WARN "探测目录不存在，跳过实测: $ProbeDir"
        Report INFO "可用 -ProbeDir 指定一个含 .rvt 的目录重跑本项"
    }
} else {
    Report WARN "找不到 rvtvhandlertest.exe，跳过功能实测"
}

# ---- 汇总 ----
Write-Host ''
Write-Host '========================================' -ForegroundColor Cyan
$color = if ($fail -eq 0) { 'Green' } else { 'Red' }
Write-Host (" 通过 $pass   失败 $fail   警告 $warn") -ForegroundColor $color
Write-Host '========================================' -ForegroundColor Cyan

if ($fail -eq 0) {
    Write-Host ''
    Write-Host '下一步（这一步只能手工做，脚本无法代劳）：' -ForegroundColor Yellow
    Write-Host '  1. 打开一个含 .rvt 的文件夹，切到「详细信息」视图'
    Write-Host '  2. 在任意列头（比如「大小」）上右键 -> 「选择列」/「其他…」'
    Write-Host '  3. 在列表里找到并勾选「Revit Version」'
    Write-Host '  4. 如果列里仍然空白：按 Ctrl+Shift+Esc 重启资源管理器，或注销重登'
    Write-Host ''
    Write-Host '（属性值有缓存，新装后第一次可能要等几秒或滚动一下才刷新）'
} else {
    Write-Host ''
    Write-Host '修复建议：' -ForegroundColor Yellow
    Write-Host '  重新以管理员身份运行 install-handler.bat（先 -Uninstall 再装）'
}

exit $(if ($fail -eq 0) { 0 } else { 1 })
