@echo off
cd /d "%~dp0"
if not exist "dist\LunaCapture.exe" (
    echo LunaCapture.exe is missing. Run install_and_run.bat first.
    pause
    exit /b 1
)
start "" "dist\LunaCapture.exe"
