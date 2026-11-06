@echo off
setlocal
set "TARGET_DIR=%~dp0build_x86"
if not exist "%TARGET_DIR%\openkj.exe" (
    echo OpenKJ 32-bit executable not found in build_x86!
    pause
    exit /b 1
)
set "PATH=%TARGET_DIR%;%PATH%"
cd /d "%TARGET_DIR%"
start "" "%TARGET_DIR%\openkj.exe" %*
exit /b 0
