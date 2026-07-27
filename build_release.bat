@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem Single source of truth: the VERSION file at the repo root.
set "APP_VERSION=unknown"
if exist "VERSION" set /p APP_VERSION=<"VERSION"

echo.
echo  Luna Capture %APP_VERSION% - Native Release Build
echo  =============================================
echo.

where winget.exe >nul 2>nul
if errorlevel 1 (
    set "HAS_WINGET=0"
) else (
    set "HAS_WINGET=1"
)

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

set "VSVERSION="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion`) do set "VSVERSION=%%I"
set "CMAKE_VS_GENERATOR=Visual Studio 17 2022"
if "%VSVERSION:~0,2%"=="18" set "CMAKE_VS_GENERATOR=Visual Studio 18 2026"
set "CMAKE_GENERATOR_LABEL=%CMAKE_VS_GENERATOR%"
set CMAKE_GENERATOR_ARGS=-G "%CMAKE_VS_GENERATOR%" -A x64
set "VS_NINJA_DIR=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if exist "%VS_NINJA_DIR%\ninja.exe" set "PATH=%VS_NINJA_DIR%;%PATH%"
where ninja.exe >nul 2>nul
if not errorlevel 1 (
    set "CMAKE_GENERATOR_LABEL=Ninja + MSVC"
    set "CMAKE_GENERATOR_ARGS=-G Ninja -DCMAKE_BUILD_TYPE=Release"
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
echo       Generator: %CMAKE_GENERATOR_LABEL%
cmake -S . -B build %CMAKE_GENERATOR_ARGS% -DBUILD_TESTING=ON
if errorlevel 1 goto :failed

echo [2/4] Compiling native C++ sources...
cmake --build build --config Release --parallel
if errorlevel 1 goto :failed

echo [3/4] Running core tests...
ctest --test-dir build -C Release --output-on-failure
if errorlevel 1 goto :failed

echo [4/5] Copying portable executable...
copy /y "build\bin\LunaCapture.exe" "dist\LunaCapture.exe" >nul
if errorlevel 1 goto :failed
rem The installer/uninstaller ships alongside the portable build.
copy /y "build\bin\LunaSetup.exe" "dist\LunaSetup.exe" >nul
if errorlevel 1 goto :failed
copy /y "installer\installer.ini" "dist\installer.ini" >nul
if errorlevel 1 goto :failed
if exist "dist\docs\examples" rmdir /s /q "dist\docs\examples"
cmake -E copy_directory "docs" "dist\docs"
if errorlevel 1 goto :failed
>"dist\version.txt" echo %APP_VERSION%
rem Keep the website badges in sync with the VERSION file.
>"website\version.js" echo window.LUNA_VERSION="%APP_VERSION%";document.addEventListener("DOMContentLoaded",function(){document.querySelectorAll(".app-version").forEach(function(el){el.textContent=window.LUNA_VERSION;});});

echo [5/5] Packaging LunaCapture-portable.zip...
rem This is the exact asset name LunaSetup.exe looks for on the release page.
rem It holds the app plus its offline docs - not the installer itself.
set "PORTABLE_STAGE=%TEMP%\LunaCapturePortable"
if exist "%PORTABLE_STAGE%" rmdir /s /q "%PORTABLE_STAGE%"
mkdir "%PORTABLE_STAGE%"
copy /y "dist\LunaCapture.exe" "%PORTABLE_STAGE%\LunaCapture.exe" >nul
if errorlevel 1 goto :failed
copy /y "dist\version.txt" "%PORTABLE_STAGE%\version.txt" >nul
cmake -E copy_directory "docs" "%PORTABLE_STAGE%\docs"
if errorlevel 1 goto :failed
if exist "license.txt" copy /y "license.txt" "%PORTABLE_STAGE%\license.txt" >nul
del /q "dist\LunaCapture-portable.zip" >nul 2>nul
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%PORTABLE_STAGE%\*' -DestinationPath 'dist\LunaCapture-portable.zip' -Force"
if errorlevel 1 goto :failed
rmdir /s /q "%PORTABLE_STAGE%"
if not exist "dist\LunaCapture-portable.zip" goto :failed

for %%F in ("dist\LunaCapture.exe") do echo [OK] %%~fF  ^(%%~zF bytes^)
for %%F in ("dist\LunaSetup.exe") do echo [OK] %%~fF  ^(%%~zF bytes^)
for %%F in ("dist\LunaCapture-portable.zip") do echo [OK] %%~fF  ^(%%~zF bytes^)
echo.
echo Attach dist\LunaCapture-portable.zip and dist\LunaSetup.exe to the GitHub release.
echo Build completed successfully.
exit /b 0

:failed
echo.
echo [ERROR] Build failed. Scroll to the first compiler or linker error above.
pause
exit /b 1
