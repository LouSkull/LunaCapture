@echo off
setlocal
cd /d "%~dp0"
echo === Luna Capture 1.0.0.0 diagnostics ===
echo.
echo Operating system:
ver
echo.
echo CMake:
where cmake.exe 2>nul
cmake --version 2>nul | findstr /i "version"
echo.
echo Visual Studio C++ toolchain:
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
) else (
  echo vswhere.exe not found
)
echo.
echo Built executable:
if exist "dist\LunaCapture.exe" (dir "dist\LunaCapture.exe") else (echo Not built yet)
echo.
echo Settings:
echo %LOCALAPPDATA%\Luna Capture\settings.ini
echo.
pause
