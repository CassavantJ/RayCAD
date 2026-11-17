@echo off
rem Configure and build one spike with MSVC + Ninja (Release).
rem Usage: spikes\build.cmd <spike-folder-name> [extra cmake configure args]
rem Example: spikes\build.cmd s8-quickjs -DFOO=ON
setlocal
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "SPIKE=%~dp0%~1"
shift
cmake -S "%SPIKE%" -B "%SPIKE%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release %1 %2 %3 %4 %5 %6 || exit /b 1
cmake --build "%SPIKE%\build" || exit /b 1
