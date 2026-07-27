@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "scripts\stop_old_lunashot.ps1"
if errorlevel 1 (
    echo [ERROR] Exit the elevated Luna Capture tray instance, then run this installer again.
    pause
    exit /b 2
)
if exist "dist\LunaCapture.running-old.exe" del /q "dist\LunaCapture.running-old.exe" >nul 2>nul

set "NEED_BUILD=0"
if not exist "dist\LunaCapture.exe" set "NEED_BUILD=1"
if not exist "dist\version.txt" set "NEED_BUILD=1"
if exist "dist\version.txt" (
    set /p BUILT_VERSION=<"dist\version.txt"
    if /i not "!BUILT_VERSION!"=="1.0.0.0" set "NEED_BUILD=1"
)

if "!NEED_BUILD!"=="0" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "scripts\needs_rebuild.ps1"
    if errorlevel 1 set "NEED_BUILD=1"
)

if "!NEED_BUILD!"=="1" (
    call build_release.bat
    if errorlevel 1 exit /b 1
)

start "" "dist\LunaCapture.exe"
exit /b 0
