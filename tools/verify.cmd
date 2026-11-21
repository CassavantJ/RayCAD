@echo off
rem Configures, builds and tests RayCAD on Windows. One argument picks how much (README, "Which tests to run"):
rem   quick  (default) Release build, then the quick test tier (ctest --preset windows-msvc-release-quick). Every merge.
rem   full   Release build, then every test (windows-msvc-release). Before a push, and after any change in src/naming,
rem          src/kernel, ports/ or the naming oracle.
rem   ci     exactly what .github/workflows/build-test.yml does on Windows: vcpkg install (triplet x64-windows, into
rem          build\vcpkg_installed), configure, Debug build and the quick tier, Release build and every test.
rem It stops at the first step that fails. ctest runs one job per logical processor, as CI does (tests/tiers.cmake).
rem
rem Usage, from a plain cmd prompt in any folder:   tools\verify.cmd [quick, full or ci]
rem
rem Read from the environment (nothing is written back to any file):
rem   VCPKG_ROOT                  required: a vcpkg checkout that has the baseline commit of vcpkg-configuration.json
rem   QT_ROOT_DIR                 required: the Qt 6.12 kit folder, e.g. C:\Qt\6.12.0\msvc2022_64
rem   VCPKG_DEFAULT_BINARY_CACHE  optional: vcpkg's binary cache folder (built packages kept between build trees)
rem   RAY_VCPKG_INSTALL_OPTIONS   optional: more options for vcpkg install, e.g. --x-buildtrees-root=C:\vb when the
rem                               checkout's path is too long for OpenCASCADE's build tree. Not part of CI.
rem tools\win-env.cmd sets up Visual Studio's compiler, CMake and Ninja; it is called here, and VCPKG_ROOT is kept
rem as given (Visual Studio's own environment script points it at the vcpkg that ships with Visual Studio).
rem
rem Exit code: 0 everything passed, 1 environment or usage, 2 vcpkg install, 3 configure, 4 Debug build, 5 Debug
rem tests, 6 Release build, 7 Release tests.
setlocal
cd /d "%~dp0.."

set "RAY_VERIFY_MODE=%~1"
if "%RAY_VERIFY_MODE%"=="" set "RAY_VERIFY_MODE=quick"
if /i not "%RAY_VERIFY_MODE%"=="quick" if /i not "%RAY_VERIFY_MODE%"=="full" if /i not "%RAY_VERIFY_MODE%"=="ci" (
    echo verify: unknown mode "%RAY_VERIFY_MODE%"; use quick, full or ci 1>&2
    exit /b 1
)
if not defined VCPKG_ROOT (
    echo verify: set VCPKG_ROOT to your vcpkg checkout 1>&2
    exit /b 1
)
if not defined QT_ROOT_DIR (
    echo verify: set QT_ROOT_DIR to the Qt 6.12 kit folder 1>&2
    exit /b 1
)
set "RAY_VERIFY_VCPKG_ROOT=%VCPKG_ROOT%"
call tools\win-env.cmd || exit /b 1
set "VCPKG_ROOT=%RAY_VERIFY_VCPKG_ROOT%"
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
    echo verify: %VCPKG_ROOT%\vcpkg.exe not found; bootstrap vcpkg first 1>&2
    exit /b 1
)

if /i "%RAY_VERIFY_MODE%"=="ci" (
    echo === verify ci: vcpkg install
    "%VCPKG_ROOT%\vcpkg.exe" install --triplet x64-windows --x-install-root=build/vcpkg_installed %RAY_VCPKG_INSTALL_OPTIONS% || exit /b 2
)

echo === verify %RAY_VERIFY_MODE%: configure
if defined RAY_VCPKG_INSTALL_OPTIONS (
    cmake --preset windows-msvc "-DVCPKG_INSTALL_OPTIONS=%RAY_VCPKG_INSTALL_OPTIONS%" || exit /b 3
) else (
    cmake --preset windows-msvc || exit /b 3
)

if /i "%RAY_VERIFY_MODE%"=="ci" (
    echo === verify ci: Debug build
    cmake --build --preset windows-msvc-debug || exit /b 4
    echo === verify ci: Debug tests, quick tier
    ctest --preset windows-msvc-debug-quick --parallel %NUMBER_OF_PROCESSORS% || exit /b 5
)

echo === verify %RAY_VERIFY_MODE%: Release build
cmake --build --preset windows-msvc-release || exit /b 6
if /i "%RAY_VERIFY_MODE%"=="quick" (
    echo === verify quick: Release tests, quick tier
    ctest --preset windows-msvc-release-quick --parallel %NUMBER_OF_PROCESSORS% || exit /b 7
) else (
    echo === verify %RAY_VERIFY_MODE%: Release tests, every tier
    ctest --preset windows-msvc-release --parallel %NUMBER_OF_PROCESSORS% || exit /b 7
)

echo === verify %RAY_VERIFY_MODE%: passed
exit /b 0
