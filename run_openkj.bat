@echo off
setlocal

set "IS_64BIT=0"
if /I "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "IS_64BIT=1"
if defined PROCESSOR_ARCHITEW6432 set "IS_64BIT=1"

set "TARGET_DIR="
if "%IS_64BIT%"=="1" (
    if exist "%~dp0build\openkj.exe" set "TARGET_DIR=%~dp0build"
)
if not defined TARGET_DIR (
    if exist "%~dp0build_x86\openkj.exe" set "TARGET_DIR=%~dp0build_x86"
)
if not defined TARGET_DIR (
    if exist "%~dp0build\openkj.exe" set "TARGET_DIR=%~dp0build"
)

if not defined TARGET_DIR (
    echo OpenKJ executable not found in build or build_x86!
    pause
    exit /b 1
)

set "PATH=%TARGET_DIR%;%PATH%"
cd /d "%TARGET_DIR%"
start "" "%TARGET_DIR%\openkj.exe" %*
exit /b 0
