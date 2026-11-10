@echo off
setlocal EnableExtensions

rem MPC-VR-MFG x64 Release build.
rem Default: rebuild the whole solution (including PropPage.cpp and .rc resources).
rem Optional: "quick" uses MSBuild incremental compilation.

pushd "%~dp0" || (
    echo [ERROR] Could not open the project directory.
    exit /b 1
)

set "ACTION=Rebuild"
if /I "%~1"=="quick" set "ACTION=Build"
if /I "%~1"=="rebuild" set "ACTION=Rebuild"
if not "%~1"=="" if /I not "%~1"=="quick" if /I not "%~1"=="rebuild" goto :usage

if not exist "MpcVideoRenderer.sln" (
    echo [ERROR] MpcVideoRenderer.sln not found.
    goto :fail
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] Visual Studio vswhere.exe not found.
    goto :fail
)

set "MFG_VSROOT="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "MFG_VSROOT=%%I"
if not defined MFG_VSROOT (
    echo [ERROR] No Visual Studio installation with the MSVC x64 tools was found.
    goto :fail
)

set "MSBUILD=%MFG_VSROOT%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" (
    echo [ERROR] MSBuild.exe not found: "%MSBUILD%"
    goto :fail
)

rem A fresh Git clone has no proprietary NVIDIA SDK files. Fetch the pinned,
rem SHA256-checked build-only dependencies automatically when building.
rem This does not install any runtime DLL or register the renderer.
if not exist "build-support\get-nvidia-build-deps.ps1" (
    echo [ERROR] NVIDIA dependency bootstrap script is missing.
    goto :fail
)
if not exist "build-support\nvidia-build-dependencies.json" (
    echo [ERROR] NVIDIA dependency manifest is missing.
    goto :fail
)
where.exe powershell.exe >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Windows PowerShell is required to fetch NVIDIA build dependencies.
    goto :fail
)
echo [INFO] Verifying NVIDIA build dependencies ^(auto-download if needed^)...
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "build-support\get-nvidia-build-deps.ps1"
if errorlevel 1 (
    echo [ERROR] NVIDIA build dependency retrieval/verification failed.
    echo [ERROR] See the PowerShell error above for the specific cause.
    goto :fail
)
if not exist "vendor\nvidia\include\nvsdk_ngx.h" (
    echo [ERROR] Required NVIDIA SDK header is still missing after bootstrap.
    goto :fail
)
if not exist "vendor\nvidia\lib\nvsdk_ngx_s.lib" (
    echo [ERROR] Required NVIDIA SDK import library is still missing after bootstrap.
    goto :fail
)

if not exist "artifacts\build" mkdir "artifacts\build"
if not exist "artifacts\build" (
    echo [ERROR] Could not create artifacts\build.
    goto :fail
)

set "LOGFILE=%CD%\artifacts\build\x64-release.log"
echo.
echo ================================================
echo   MPC-VR-MFG - Release x64 - %ACTION%
echo ================================================
echo Solution: "%CD%\MpcVideoRenderer.sln"
echo Log:      "%LOGFILE%"
echo.

rem /t:Rebuild also recompiles the property pages and their .rc resources.
rem No timestamp tricks, manual .obj deletion, or registry changes are required.
"%MSBUILD%" "MpcVideoRenderer.sln" /t:%ACTION% /m:2 /nr:false /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal /fl "/flp:logfile=%LOGFILE%;verbosity=normal;encoding=UTF-8"
if errorlevel 1 (
    echo [ERROR] MSBuild failed. See "%LOGFILE%".
    goto :fail
)

set "FILTER=_bin\Filter_x64\MPCVRMFG64.ax"
if not exist "%FILTER%" (
    echo [ERROR] MSBuild returned success, but "%FILTER%" was not found.
    goto :fail
)

rem Produce a clean, self-contained local distribution after a successful build.
rem The verified NVIDIA runtime is obtained from local cache or NVIDIA's official
rem pinned release. Registration is deliberately never run by this build.
if not exist "build-support\package-release.ps1" (
    echo [ERROR] Distribution packaging script is missing.
    goto :fail
)
echo [INFO] Preparing root-level dist folder...
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "build-support\package-release.ps1"
if errorlevel 1 (
    echo [ERROR] Distribution packaging failed. Compiled outputs remain in _bin.
    goto :fail
)

echo.
echo [OK] %ACTION% completed successfully.
for %%F in ("%FILTER%") do echo Compiled: %%~fF  [%%~zF bytes]
echo Distribution: "%CD%\dist"
echo.
echo This script only builds files. It does not register the AX or launch MPC-BE.
popd
exit /b 0

:usage
echo Usage: build-x64.cmd [rebuild^|quick]
echo   (no argument)   Full solution rebuild, safest after applying a ZIP patch.
echo   rebuild         Same as default.
echo   quick           Incremental build, faster for ordinary local edits.
popd
exit /b 2

:fail
popd
exit /b 1
