@echo off
rem Configure and build one spike with MSVC + Ninja (Release).
rem Usage: spikes\build.cmd <spike-folder-name> [extra cmake configure args]
rem OpenCASCADE 8.0.1 comes from vcpkg at M:\dev\vcpkg (find_package(OpenCASCADE CONFIG REQUIRED)).
rem Qt 6.12.0 is at M:\Qt\6.12.0\msvc2022_64 (find_package(Qt6 ...)).
setlocal
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "SPIKE=%~dp0%~1"
shift
cmake -S "%SPIKE%" -B "%SPIKE%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=M:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_MANIFEST_MODE=OFF -DCMAKE_PREFIX_PATH=M:/Qt/6.12.0/msvc2022_64 %1 %2 %3 %4 %5 %6 || exit /b 1
cmake --build "%SPIKE%\build" || exit /b 1
