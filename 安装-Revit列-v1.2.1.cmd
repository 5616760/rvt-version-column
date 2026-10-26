@echo off
setlocal EnableDelayedExpansion
chcp 936 >nul 2>&1
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
    echo [*] 需要管理员权限，正在请求提权...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo ================================================================
echo   Revit Version 列  v1.2.1  安装
echo ================================================================
echo.

"%~dp0RvtVersionSetup.exe" /quiet
set RC=%errorlevel%

echo.
echo ================================================================
if "%RC%"=="0" (
    echo   [+] 安装成功
) else (
    echo   [x] 安装失败，返回码 %RC%
    echo       日志: %LOCALAPPDATA%\RvtVersion\setup.log
)
echo ================================================================
echo.
echo --- 自检 ---

set OK=1

if exist "%ProgramFiles%\RvtVersion\rvtvwatch.exe" (
    echo   [OK] rvtvwatch.exe
) else (
    echo   [!!] rvtvwatch.exe 缺失
    set OK=0
)

reg query "HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v RvtColumnWatcher >nul 2>&1
if errorlevel 1 (
    echo   [!!] 开机启动未注册
    set OK=0
) else (
    echo   [OK] 开机启动已注册
)

for %%E in (.rvt .rfa .rte .rft) do (
    reg query "HKLM\SOFTWARE\Classes\%%E\ShellEx\{8895b1c6-b41f-4c1c-a562-0d564250836f}" >nul 2>&1
    if errorlevel 1 (
        echo   [!!] %%E 属性处理器未注册
        set OK=0
    ) else (
        echo   [OK] %%E 属性处理器
    )
)

echo.
echo --- 监视器进程 ---
tasklist /FI "IMAGENAME eq rvtvwatch.exe" 2>nul | find /I "rvtvwatch.exe" >nul
if errorlevel 1 (
    echo   [!] 监视器未运行，正在手动拉起...
    start "" "%ProgramFiles%\RvtVersion\rvtvwatch.exe" --silent
    timeout /t 2 >nul
    tasklist /FI "IMAGENAME eq rvtvwatch.exe" 2>nul | find /I "rvtvwatch.exe" >nul
    if errorlevel 1 (
        echo   [!!] 拉起失败
    ) else (
        echo   [OK] 监视器已运行
    )
) else (
    echo   [OK] 监视器已运行
)

echo.
if "%OK%"=="1" (
    echo   全部检查通过。现在打开含 Revit 文件的文件夹即可看到列。
) else (
    echo   有检查未通过，请把上面的 [!] / [!!] 行发给我。
)
echo.
pause
endlocal
