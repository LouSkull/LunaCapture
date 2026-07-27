@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

echo.
echo  Luna Capture 1.0.0.0 - Native Release Build
echo  =============================================
echo.

where winget.exe >nul 2>nul
if errorlevel 1 set "HAS_WINGET=0" else set "HAS_WINGET=1"

where cmake.exe >nul 2>nul
if errorlevel 1 (
    if "%HAS_WINGET%"=="0" (
        echo [ERROR] CMake is missing and winget is unavailable.
        echo Install CMake 3.24 or newer, then run this file again.
        pause
        exit /b 1
    )
    echo [SETUP] Installing CMake...
    winget install --id Kitware.CMake --exact --silent --accept-source-agreements --accept-package-agreements
    set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    if "%HAS_WINGET%"=="0" (
        echo [ERROR] Visual Studio Build Tools are missing.
        echo Install Desktop development with C++ and a Windows 10 or 11 SDK.
        pause
        exit /b 1
    )
    echo [SETUP] Installing Visual Studio Build Tools and Windows SDK...
    winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --silent --accept-source-agreements --accept-package-agreements --override "--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
)

set "VSROOT="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT (
    echo [ERROR] The MSVC x64 toolchain was not found.
    echo Open Visual Studio Installer and add Desktop development with C++.
    pause
    exit /b 1
)

call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 (
    echo [ERROR] Failed to initialize the Visual Studio environment.
    pause
    exit /b 1
)

if exist build rmdir /s /q build
if not exist dist mkdir dist
del /q "dist\LunaCapture.exe" "dist\version.txt" >nul 2>nul

echo [1/4] Configuring...
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
if errorlevel 1 goto :failed

echo [2/4] Compiling native C++ sources...
cmake --build build --config Release --parallel
if errorlevel 1 goto :failed

echo [3/4] Running core tests...
ctest --test-dir build -C Release --output-on-failure
if errorlevel 1 goto :failed

echo [4/4] Copying portable executable...
copy /y "build\bin\LunaCapture.exe" "dist\LunaCapture.exe" >nul
if errorlevel 1 goto :failed
>"dist\version.txt" echo 1.0.0.0

for %%F in ("dist\LunaCapture.exe") do echo [OK] %%~fF  ^(%%~zF bytes^)
echo.
echo Build completed successfully.
exit /b 0

:failed
echo.
echo [ERROR] Build failed. Scroll to the first compiler or linker error above.
pause
exit /b 1
