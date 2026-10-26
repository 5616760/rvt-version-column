@echo off
REM =====================================================================
REM  RvtVersion 属性处理器 —— 一键安装（自动提权）
REM
REM  用 %~dp0 定位脚本目录，因此不依赖当前工作目录。
REM  注意：本文件必须为 CRLF 且【不能有 BOM】，否则 cmd 认不出 @echo off。
REM =====================================================================
setlocal EnableDelayedExpansion

set "PS1=%~dp0install-handler.ps1"
if not exist "%PS1%" (
    echo [x] 找不到 install-handler.ps1
    echo     请确认 bat 与 ps1 在同一目录
    pause
    exit /b 1
)

net session >nul 2>&1
if errorlevel 1 (
    echo [*] 需要管理员权限，正在请求提权...
    echo     若弹出 UAC 请点「是」
    REM 用 -ArgumentList 传参时，把整个 bat 路径和原始参数重新拼成一条命令，
    REM 避免参数里含空格时被拆坏。
    powershell -NoProfile -Command "try { Start-Process -FilePath '%ComSpec%' -ArgumentList '/c',\"\"\"%~f0\"\" %*\" -Verb RunAs -ErrorAction Stop } catch { Write-Host '[x] 提权被拒绝或失败: ' -ForegroundColor Red; Write-Host $_.Exception.Message; Start-Sleep -Seconds 4 }"
    exit /b
)

echo [*] 以管理员身份运行
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
    echo [*] 完成。按任意键关闭...
) else (
    echo [x] 退出码 %RC%，请查看上方输出
)
pause >nul
exit /b %RC%
