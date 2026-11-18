@echo off
rem Start the RayCAD desktop app from a local Windows build (preset windows-msvc, Release by default).
rem
rem   tools\raycad.cmd [part.step] [raycad options]      set RAYCAD_CONFIG=Debug for the debug build
rem
rem The build does not copy the Qt and OpenCASCADE DLLs next to the program, so this script puts their
rem folders on PATH. The Qt folder is read from the build's CMake cache.
setlocal
set "ROOT=%~dp0.."
if "%RAYCAD_CONFIG%"=="" set "RAYCAD_CONFIG=Release"
set "CACHE=%ROOT%\build\windows-msvc\CMakeCache.txt"
if not exist "%CACHE%" (
    echo raycad.cmd: no build found. Configure and build the windows-msvc preset first. 1>&2
    exit /b 1
)
if "%QT_ROOT_DIR%"=="" for /f "tokens=2 delims==" %%Q in ('findstr /b /c:"CMAKE_PREFIX_PATH:" "%CACHE%"') do set "QT_ROOT_DIR=%%Q"
set "VCPKG_BIN=%ROOT%\build\vcpkg_installed\x64-windows\bin"
if /i "%RAYCAD_CONFIG%"=="Debug" set "VCPKG_BIN=%ROOT%\build\vcpkg_installed\x64-windows\debug\bin"
set "EXE=%ROOT%\build\windows-msvc\bin\%RAYCAD_CONFIG%\raycad.exe"
if not exist "%EXE%" (
    echo raycad.cmd: %EXE% is missing. Build the %RAYCAD_CONFIG% configuration first. 1>&2
    exit /b 1
)
set "PATH=%QT_ROOT_DIR%\bin;%VCPKG_BIN%;%PATH%"
"%EXE%" %*
