# =====================================================================
# RvtVersion 属性处理器 —— 安装 / 卸载脚本
#
# 作用：让 Windows 资源管理器「详细信息」视图的列里直接显示 Revit 版本。
#
# 用法（需要管理员权限）：
#   双击 install-handler.bat
#   或 powershell -ExecutionPolicy Bypass -File install-handler.ps1
#   卸载：install-handler.bat -Uninstall
#
# 做了什么：
#   1. 复制 RvtVersionHandler.dll / RvtVersion.propdesc 到 %ProgramFiles%\RvtVersion
#   2. 注册 COM 类（HKLM\SOFTWARE\Classes\CLSID）
#   3. 注册属性描述文件（PSRegisterPropertySchema）
#   4. 把处理器挂到 .rvt / .rfa / .rte / .rft 的 ShellEx\PropertyHandler
#   5. 重启资源管理器
# =====================================================================

[CmdletBinding()]
param(
    [switch]$Uninstall,
    [switch]$NoRestart
)

$ErrorActionPreference = 'Stop'

# 脚本所在目录：卸载分支里的 Register-PropSchema 也要用，必须在任何分支之前赋值
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# ---------------------------------------------------------------- 常量
# 必须与 PropertyHandler.cpp / PropKeys.h 完全一致
$Clsid      = '{5AD3E9FC-A720-47F6-8DFA-9AACD3286E01}'
$ProgId     = 'RvtVersion.PropertyHandler'
$ProgIdOld  = 'RvtVersion.VersionOverlay'

$DllName    = 'RvtVersionHandler.dll'
$PropDesc   = 'RvtVersion.propdesc'

$TargetDir  = Join-Path $env:ProgramFiles 'RvtVersion'
$CacheDir   = Join-Path $env:LOCALAPPDATA 'RvtVersion'

# 要挂属性处理器的扩展名
$Extensions = @('.rvt', '.rfa', '.rte', '.rft')

# PropertyHandler 的 ShellEx 子键 GUID（由系统定义，不是我们的 CLSID）
$ShellExPropHandler = '{8895b1c6-b41f-4c1c-a562-0d564250836f}'

# ---------------------------------------------------------------- 工具函数
# 写注册表默认值必须用 Set-Item -Value，不能用 Set-ItemProperty -Name '(default)'。
# 后者会创建一个名字就叫 "(default)" 的普通值，真正的默认值仍为空，
# 导致 CLSID\InprocServer32 与 ShellEx\... 全部失效 —— DLL 加载不到，列里什么都不显示。
function Set-RegDefault {
    param([Parameter(Mandatory)][string]$Path,
          [Parameter(Mandatory)][AllowEmptyString()][string]$Value)
    Set-Item -Path $Path -Value $Value
}

# 读注册表默认值（HKCU / HKLM 都支持）
function Get-RegDefault {
    param([Parameter(Mandatory)][string]$Path)
    if ($Path -like 'HKLM:*') {
        $hive = [Microsoft.Win32.Registry]::LocalMachine
    } else {
        $hive = [Microsoft.Win32.Registry]::CurrentUser
    }
    $sub = $Path -replace '^HK(LM|CU):\\', ''
    $key = $hive.OpenSubKey($sub)
    if (-not $key) { return $null }
    try { return $key.GetValue('') } finally { $key.Close() }
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Restart-Explorer {
    param([switch]$Skip)
    if ($Skip) {
        Write-Host '  [=] 已指定 -NoRestart，跳过重启资源管理器'
        return
    }
    Write-Host '  [*] 重启资源管理器...'
    $p = Get-Process -Name explorer -ErrorAction SilentlyContinue
    if ($p) {
        $p | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 1500
    }
    if (-not (Get-Process -Name explorer -ErrorAction SilentlyContinue)) {
        Start-Process explorer.exe
    }
    Write-Host '  [+] 资源管理器已重启'
}

# 用自带的 rvtvpropschema.exe 调 PSRegisterPropertySchema。
# 不用 Add-Type 的原因：部分环境（含安全加固的 PowerShell）会拦截运行时编译，
# 独立进程更稳，且卸载/验证走同一套代码。
function Register-PropSchema {
    param([string]$Path, [switch]$Remove)

    # 卸载时 TargetDir 里还有工具；安装时先查 build.bat 的产物目录
    $tool = Join-Path $TargetDir 'rvtvpropschema.exe'
    if (-not (Test-Path $tool)) {
        $tool = Join-Path $scriptDir 'src\RevitVersion.Native\out\rvtvpropschema.exe'
    }
    if (-not (Test-Path $tool)) {
        $tool = Join-Path $scriptDir 'out\rvtvpropschema.exe'
    }
    if (-not (Test-Path $tool)) {
        throw "找不到 rvtvpropschema.exe，无法注册属性描述"
    }

    $arg = if ($Remove) { '--unregister' } else { '--register' }
    $out = & $tool $arg $Path 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "属性描述注册失败: $out"
    }
    Write-Host "    $out"
}

# ---------------------------------------------------------------- 权限
if (-not (Test-Admin)) {
    Write-Host '[x] 需要管理员权限。' -ForegroundColor Red
    Write-Host '    请右键 install-handler.bat -> 以管理员身份运行'
    exit 1
}

# =====================================================================
# 卸载
# =====================================================================
if ($Uninstall) {
    Write-Host '=== 卸载 RvtVersion 属性处理器 ===' -ForegroundColor Cyan

    # 1. 摘掉文件类型上的处理器（只删我们自己的）
    foreach ($ext in $Extensions) {
        $key = "HKLM:\SOFTWARE\Classes\$ext\ShellEx\$ShellExPropHandler"
        if (Test-Path $key) {
            $cur = Get-RegDefault $key
            if ($cur -eq $Clsid) {
                Remove-Item $key -Recurse -Force
                Write-Host "  [-] 移除 $ext 的属性处理器"
            } else {
                Write-Host "  [=] $ext 的处理器不是本工具（$cur），跳过"
            }
        }
        # 旧版可能写在 ProgId 下，一并清理
        foreach ($progId in @('Revit.Project', 'Revit.Family', 'Revit.Template')) {
            $pk = "HKLM:\SOFTWARE\Classes\$progId\ShellEx\$ShellExPropHandler"
            if (Test-Path $pk) {
                $v = Get-RegDefault $pk
                if ($v -eq $Clsid) {
                    Remove-Item $pk -Recurse -Force
                    Write-Host "  [-] 移除 $progId 的属性处理器"
                }
            }
        }
    }

    # 2. 清掉预览窗格 / 悬停提示
    foreach ($ext in $Extensions) {
        $sfa = "HKLM:\SOFTWARE\Classes\SystemFileAssociations\$ext"
        if (Test-Path $sfa) {
            foreach ($n in @('PreviewDetails', 'InfoTip')) {
                $v = (Get-ItemProperty $sfa -Name $n -ErrorAction SilentlyContinue).$n
                if ($v -and $v -like '*RvtVersion.*') {
                    Remove-ItemProperty -Path $sfa -Name $n -Force
                    Write-Host "  [-] 移除 $ext 的 $n"
                }
            }
        }
    }

    # 3. 注销属性描述
    $pdTarget = Join-Path $TargetDir $PropDesc
    if (Test-Path $pdTarget) {
        Register-PropSchema -Path $pdTarget -Remove
        Write-Host '  [-] 注销属性描述'
    }

    # 4. 删 CLSID
    foreach ($base in @('HKLM:\SOFTWARE\Classes\CLSID',
                        'HKLM:\SOFTWARE\Wow6432Node\Classes\CLSID')) {
        $p = Join-Path $base $Clsid
        if (Test-Path $p) {
            Remove-Item $p -Recurse -Force
            Write-Host "  [-] 删除 CLSID: $p"
        }
    }

    foreach ($pid_ in @($ProgId, $ProgIdOld)) {
        $p = "HKLM:\SOFTWARE\Classes\$pid_"
        if (Test-Path $p) {
            Remove-Item $p -Recurse -Force
            Write-Host "  [-] 删除 ProgID: $pid_"
        }
    }

    # 5. 删文件与缓存
    if (Test-Path $TargetDir) {
        Remove-Item $TargetDir -Recurse -Force
        Write-Host "  [-] 删除安装目录: $TargetDir"
    }
    if (Test-Path $CacheDir) {
        Remove-Item $CacheDir -Recurse -Force
        Write-Host "  [-] 删除缓存: $CacheDir"
    }


    Restart-Explorer -Skip:$NoRestart
    Write-Host '=== 卸载完成 ===' -ForegroundColor Green
    exit 0
}

# =====================================================================
# 安装
# =====================================================================
Write-Host '=== 安装 RvtVersion 属性处理器 ===' -ForegroundColor Cyan

# ---- 1. 定位源文件 ----
# 优先用 src\RevitVersion.Native\out（build.bat 的产物目录）
$candidates = @(
    (Join-Path $scriptDir 'src\RevitVersion.Native\out'),
    (Join-Path $scriptDir 'out'),
    (Join-Path $scriptDir 'bin'),
    $scriptDir
)

$srcDir = $null
foreach ($c in $candidates) {
    if (Test-Path (Join-Path $c $DllName)) { $srcDir = $c; break }
}
if (-not $srcDir) {
    Write-Host "[x] 找不到 $DllName" -ForegroundColor Red
    Write-Host '    请先执行: src\RevitVersion.Native\build.bat'
    exit 1
}
Write-Host "  源目录: $srcDir"

# ---- 2. 复制到 Program Files ----
if (-not (Test-Path $TargetDir)) {
    New-Item -ItemType Directory -Path $TargetDir -Force | Out-Null
}
foreach ($f in @($DllName, $PropDesc, 'rvtvpropschema.exe')) {
    $s = Join-Path $srcDir $f
    if (-not (Test-Path $s)) {
        Write-Host "  [x] 缺少 $f，安装中止" -ForegroundColor Red
        exit 1
    }
    Copy-Item $s -Destination $TargetDir -Force
    Write-Host "  [+] 复制 $f"
}

$dllPath = Join-Path $TargetDir $DllName
$pdPath  = Join-Path $TargetDir $PropDesc

# 所有用户可读（资源管理器以普通用户身份加载 DLL）
try {
    $acl = Get-Acl $TargetDir
    $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        'Users', 'ReadAndExecute', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
    $acl.AddAccessRule($rule)
    Set-Acl $TargetDir $acl
    Write-Host '  [+] 设置目录读取权限'
} catch {
    Write-Host "  [!] 设置权限失败: $_"
}

# ---- 3. 注册 COM 类 ----
$clsidKey = "HKLM:\SOFTWARE\Classes\CLSID\$Clsid"
New-Item -Path $clsidKey -Force | Out-Null
Set-RegDefault -Path $clsidKey -Value 'RvtVersion Revit Version Property Handler'

$inproc = "$clsidKey\InprocServer32"
New-Item -Path $inproc -Force | Out-Null
Set-RegDefault -Path $inproc -Value $dllPath
Set-ItemProperty -Path $inproc -Name 'ThreadingModel' -Value 'Both'

$progKey = "HKLM:\SOFTWARE\Classes\$ProgId"
New-Item -Path $progKey -Force | Out-Null
Set-RegDefault -Path $progKey -Value 'RvtVersion Revit Version Property Handler'
New-Item -Path "$progKey\CLSID" -Force | Out-Null
Set-RegDefault -Path "$progKey\CLSID" -Value $Clsid

New-Item -Path "$clsidKey\ProgID" -Force | Out-Null
Set-RegDefault -Path "$clsidKey\ProgID" -Value $ProgId

Write-Host "  [+] 注册 CLSID: $Clsid"

# ---- 4. 注册属性描述 ----
Register-PropSchema -Path $pdPath
Write-Host '  [+] 注册属性描述 schema'

# ---- 5. 挂到文件类型 ----
foreach ($ext in $Extensions) {
    $extKey = "HKLM:\SOFTWARE\Classes\$ext"
    if (-not (Test-Path $extKey)) {
        New-Item -Path $extKey -Force | Out-Null
        Write-Host "  [!] $ext 原本未关联，已创建空键"
    }

    $seKey = "$extKey\ShellEx\$ShellExPropHandler"
    if (Test-Path $seKey) {
        $old = Get-RegDefault $seKey
        if ($old -and $old -ne $Clsid) {
            Write-Host "  [!] $ext 已有属性处理器 $old，将被本工具覆盖（卸载时无法自动还原）"
            # 留档，便于手工恢复
            $backup = Join-Path $CacheDir 'replaced-handlers.txt'
            if (-not (Test-Path $CacheDir)) {
                New-Item -ItemType Directory -Path $CacheDir -Force | Out-Null
            }
            Add-Content -Path $backup -Value "$ext`t$old" -Encoding UTF8
        }
    } else {
        New-Item -Path $seKey -Force | Out-Null
    }
    Set-RegDefault -Path $seKey -Value $Clsid
    Write-Host "  [+] $ext -> 属性处理器"
}

# ---- 6. 预览窗格 + 悬停提示（让版本在信息提示里也能看到）----
# PreviewDetails / InfoTip 属于 SystemFileAssociations，挂在 ShellEx\{处理器GUID}
# 下是无效位置，资源管理器不会读。（上一版就是这个错误。）
$propList = 'prop:System.ItemNameDisplay;RvtVersion.ProductYear;RvtVersion.Build;System.Size'
foreach ($ext in $Extensions) {
    $sfa = "HKLM:\SOFTWARE\Classes\SystemFileAssociations\$ext"
    if (-not (Test-Path $sfa)) { New-Item -Path $sfa -Force | Out-Null }
    Set-ItemProperty -Path $sfa -Name 'PreviewDetails' -Value $propList -ErrorAction SilentlyContinue
    Set-ItemProperty -Path $sfa -Name 'InfoTip' -Value $propList -ErrorAction SilentlyContinue
    Write-Host "  [+] $ext -> 预览窗格 + 悬停提示"
}

# ---- 7. 重启资源管理器 ----
Restart-Explorer -Skip:$NoRestart

Write-Host ''
Write-Host '=== 安装完成 ===' -ForegroundColor Green
Write-Host ''
Write-Host '现在怎么看到版本：' -ForegroundColor Yellow
Write-Host '  1. 打开含 .rvt 的文件夹，切到「详细信息」视图'
Write-Host '  2. 在列头（如「大小」）上点右键 -> 选择列（或「其他…」）'
Write-Host '  3. 勾选以下任一列：'
Write-Host '       Revit Version     <- 推荐，显示 2023 / 12.0 / 2012'
Write-Host '       Revit Year        <- 纯年份，便于排序'
Write-Host '       Revit Format      <- 数值格式号，可用于筛选 >2021'
Write-Host '       Revit Build       <- 构建号'
Write-Host '       Revit Worksharing <- Central / Workshared'
Write-Host ''
Write-Host '验证注册是否到位：' -ForegroundColor Yellow
Write-Host "  Get-ItemProperty 'HKLM:\SOFTWARE\Classes\.rvt\ShellEx\$ShellExPropHandler'"
Write-Host "  Get-ItemProperty 'HKLM:\SOFTWARE\Classes\CLSID\$Clsid\InprocServer32'"
Write-Host ''
Write-Host '卸载：install-handler.bat -Uninstall'
