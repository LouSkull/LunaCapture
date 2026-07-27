@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem Builds a clean "github-source" folder holding only the files that belong in
rem the repository - no build output, no dist, no captures, no local artifacts.
rem Useful because GitHub's web uploader takes at most 100 files per drag.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "scripts\prepare_github_upload.ps1"
if errorlevel 1 (
    echo.
    echo [ERROR] Staging failed. See the message above.
    pause
    exit /b 1
)

echo Opening the staged folder...
if exist "github-source" start "" explorer "%CD%\github-source"
pause
exit /b 0
