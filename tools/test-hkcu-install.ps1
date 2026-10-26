# =====================================================================
# HKCU 模拟安装测试
#
# 目的：在不需要管理员权限的前提下，验证 install-handler.ps1 的注册表
#       写入逻辑（尤其是「默认值必须用 Set-Item」这个修复）是否正确。
#
# 做法：把 install-handler.ps1 里的关键写键逻辑用同一套 Set-RegDefault /
#       Get-RegDefault 函数在 HKCU\Software\Classes 下重放一遍，然后逐项断言。
#
# 注意：这不是真安装。HKCU\Software\Classes 对资源管理器也有效，但属性描述
#       schema 只能注册到 HKLM，所以这里只验证键与默认值结构。
# =====================================================================

$ErrorActionPreference = 'Stop'

$Clsid  = '{5AD3E9FC-A720-47F6-8DFA-9AACD3286E01}'
$ProgId = 'RvtVersion.PropertyHandler'
$ShellExPropHandler = '{8895b1c6-b41f-4c1c-a562-0d564250836f}'
$Extensions = @('.rvt', '.rfa', '.rte', '.rft')

$Root = 'HKCU:\Software\Classes'
$TestRoot = 'HKCU:\Software\__rvtv_installtest'

# ---- 与 install-handler.ps1 完全相同的两个函数 ----
function Set-RegDefault {
    param([Parameter(Mandatory)][string]$Path,
          [Parameter(Mandatory)][AllowEmptyString()][string]$Value)
    Set-Item -Path $Path -Value $Value
}
function Get-RegDefault {
    param([Parameter(Mandatory)][string]$Path)
    if ($Path -like 'HKLM:*') { $hive = [Microsoft.Win32.Registry]::LocalMachine }
    else { $hive = [Microsoft.Win32.Registry]::CurrentUser }
    $sub = $Path -replace '^HK(LM|CU):\\', ''
    $key = $hive.OpenSubKey($sub)
    if (-not $key) { return $null }
    try { return $key.GetValue('') } finally { $key.Close() }
}

# ---- 清理 ----
if (Test-Path $TestRoot) { Remove-Item $TestRoot -Recurse -Force }
New-Item -Path $TestRoot -Force | Out-Null

# =====================================================================
# 复现安装脚本的写键序列，全部改到 TestRoot 下
# =====================================================================
$dllPath = 'C:\Program Files\RvtVersion\RvtVersionHandler.dll'

# COM 类
$clsidKey = "$TestRoot\CLSID\$Clsid"
New-Item -Path $clsidKey -Force | Out-Null
Set-RegDefault -Path $clsidKey -Value 'RvtVersion Revit Version Property Handler'

$inproc = "$clsidKey\InprocServer32"
New-Item -Path $inproc -Force | Out-Null
Set-RegDefault -Path $inproc -Value $dllPath
Set-ItemProperty -Path $inproc -Name 'ThreadingModel' -Value 'Both'

$progKey = "$TestRoot\$ProgId"
New-Item -Path $progKey -Force | Out-Null
Set-RegDefault -Path $progKey -Value 'RvtVersion Revit Version Property Handler'
New-Item -Path "$progKey\CLSID" -Force | Out-Null
Set-RegDefault -Path "$progKey\CLSID" -Value $Clsid

New-Item -Path "$clsidKey\ProgID" -Force | Out-Null
Set-RegDefault -Path "$clsidKey\ProgID" -Value $ProgId

# 文件类型挂载
foreach ($ext in $Extensions) {
    $extKey = "$TestRoot\$ext"
    if (-not (Test-Path $extKey)) { New-Item -Path $extKey -Force | Out-Null }
    $seKey = "$extKey\ShellEx\$ShellExPropHandler"
    New-Item -Path $seKey -Force | Out-Null
    Set-RegDefault -Path $seKey -Value $Clsid
}

# =====================================================================
# 断言
# =====================================================================
$fail = 0
function Check {
    param([string]$Label, [string]$Actual, [string]$Expect)
    if ($Actual -eq $Expect) {
        Write-Host ("  [PASS] $Label = $Actual") -ForegroundColor Green
    } else {
        Write-Host ("  [FAIL] $Label : got [$Actual] expect [$Expect]") -ForegroundColor Red
        $script:fail++
    }
}

Write-Host ''
Write-Host '=== 断言：默认值 ===' -ForegroundColor Cyan
Check 'CLSID default'          (Get-RegDefault $clsidKey)                          'RvtVersion Revit Version Property Handler'
Check 'InprocServer32 default' (Get-RegDefault $inproc)                           $dllPath
Check 'ProgID default'         (Get-RegDefault $progKey)                          'RvtVersion Revit Version Property Handler'
Check 'ProgID\CLSID default'   (Get-RegDefault "$progKey\CLSID")                  $Clsid
Check 'CLSID\ProgID default'   (Get-RegDefault "$clsidKey\ProgID")                $ProgId

foreach ($ext in $Extensions) {
    Check "$ext ShellEx default" (Get-RegDefault "$TestRoot\$ext\ShellEx\$ShellExPropHandler") $Clsid
}

Write-Host ''
Write-Host '=== 断言：不能出现名为 (default) 的垃圾值 ===' -ForegroundColor Cyan
$keys = @($clsidKey, $inproc, $progKey, "$progKey\CLSID", "$clsidKey\ProgID")
foreach ($ext in $Extensions) { $keys += "$TestRoot\$ext\ShellEx\$ShellExPropHandler" }

$junk = 0
foreach ($k in $keys) {
    $hive = [Microsoft.Win32.Registry]::CurrentUser
    $sub = $k -replace '^HKCU:\\', ''
    $key = $hive.OpenSubKey($sub)
    if ($key) {
        $names = $key.GetValueNames()
        $key.Close()
        if ($names -contains '(default)') {
            Write-Host ("  [FAIL] $k 含垃圾值 '(default)'") -ForegroundColor Red
            $junk++
        }
    }
}
if ($junk -eq 0) {
    Write-Host '  [PASS] 无垃圾 (default) 值' -ForegroundColor Green
} else {
    $fail += $junk
}

Write-Host ''
Write-Host '=== 断言：ThreadingModel ===' -ForegroundColor Cyan
$tm = (Get-ItemProperty $inproc -Name 'ThreadingModel').ThreadingModel
Check 'InprocServer32 ThreadingModel' $tm 'Both'

# ---- 清理 ----
Remove-Item $TestRoot -Recurse -Force

Write-Host ''
if ($fail -eq 0) {
    Write-Host '结果：全部通过' -ForegroundColor Green
    exit 0
} else {
    Write-Host "结果：$fail 项失败" -ForegroundColor Red
    exit 1
}
