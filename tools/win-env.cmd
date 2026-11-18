@echo off
rem Sets up the MSVC x64 build environment in the CURRENT cmd session (do not wrap in setlocal).
rem Usage:   call tools\win-env.cmd
rem Finds the newest Visual Studio with the C++ tools through vswhere, runs vcvars64, and puts the CMake
rem and Ninja that ship with Visual Studio on PATH if none are there already.
rem You still set, before configuring:
rem   VCPKG_ROOT    a vcpkg checkout that contains the baseline commit named in vcpkg-configuration.json
rem   QT_ROOT_DIR   the Qt 6.12 kit folder, e.g. C:\Qt\6.12.0\msvc2022_64

set "RAY_VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%RAY_VSINSTALLER%\vswhere.exe" (
    echo win-env: vswhere.exe not found; install Visual Studio with the C++ workload 1>&2
    exit /b 1
)
set "RAY_VSROOT="
for /f "usebackq delims=" %%i in (`"%RAY_VSINSTALLER%\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "RAY_VSROOT=%%i"
if not defined RAY_VSROOT (
    echo win-env: no Visual Studio installation with the x64 C++ tools was found 1>&2
    exit /b 1
)
rem vcvars64 itself looks for vswhere on PATH and prints an error line if it is not there.
set "PATH=%RAY_VSINSTALLER%;%PATH%"
call "%RAY_VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo win-env: vcvars64.bat failed 1>&2
    exit /b 1
)
where cmake >nul 2>nul
if errorlevel 1 set "PATH=%RAY_VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
where ninja >nul 2>nul
if errorlevel 1 set "PATH=%RAY_VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "RAY_VSINSTALLER="
exit /b 0
