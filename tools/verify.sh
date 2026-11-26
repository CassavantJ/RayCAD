#!/usr/bin/env bash
# Configures, builds and tests RayCAD on Linux or macOS, in this checkout. The first argument picks how much (README,
# "Which tests to run"):
#   quick  (default) Release build, then the quick test tier (ctest --preset <preset>-release-quick). Every merge.
#   full   Release build, then every test (<preset>-release). Before a push, and after any change in src/naming,
#          src/kernel, ports/ or the naming oracle.
#   ci     what .github/workflows/build-test.yml does per push and in the nightly run together: vcpkg install,
#          configure, Debug build and the quick tier, Release build and every test, with Qt's offscreen platform and
#          software scene graph.
# It stops at the first step that fails. ctest runs one job per logical processor, as CI does (tests/tiers.cmake).
#
#   tools/verify.sh [quick|full|ci] [configure preset]
#
# The preset defaults to linux-gcc on Linux and macos-arm64 on macOS. Read from the environment (nothing is written
# back to any file): VCPKG_ROOT (required: a vcpkg checkout with the baseline commit of vcpkg-configuration.json),
# QT_ROOT_DIR (required: the Qt 6.12 kit folder), VCPKG_DEFAULT_BINARY_CACHE (optional). To build on Linux from a
# Windows checkout through WSL, tools/linux-local.sh copies the tree into the distribution first.
#
# Exit code: 0 everything passed, 1 environment or usage, 2 vcpkg install, 3 configure, 4 Debug build, 5 Debug tests,
# 6 Release build, 7 Release tests.
set -uo pipefail

mode="${1:-quick}"
case "$mode" in
  quick | full | ci) ;;
  *) echo "verify: unknown mode '$mode'; use quick, full or ci" >&2; exit 1 ;;
esac
if [ "$(uname -s)" = "Darwin" ]; then
  preset="${2:-macos-arm64}"
  triplet=arm64-osx-dynamic
else
  preset="${2:-linux-gcc}"
  triplet=x64-linux-dynamic
fi
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
jobs="$(getconf _NPROCESSORS_ONLN)"
if [ -z "${VCPKG_ROOT:-}" ]; then echo "verify: set VCPKG_ROOT to your vcpkg checkout" >&2; exit 1; fi
if [ -z "${QT_ROOT_DIR:-}" ]; then echo "verify: set QT_ROOT_DIR to the Qt 6.12 kit folder" >&2; exit 1; fi

if [ "$mode" = ci ]; then
  # As the CI runners, which have no display.
  export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
  echo "=== verify ci: vcpkg install"
  "$VCPKG_ROOT/vcpkg" install --triplet "$triplet" --x-install-root=build/vcpkg_installed || exit 2
elif [ "$(uname -s)" = "Linux" ] && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
  export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" QT_QUICK_BACKEND="${QT_QUICK_BACKEND:-software}"
fi

echo "=== verify $mode: configure $preset"
cmake --preset "$preset" || exit 3
if [ "$mode" = ci ]; then
  echo "=== verify ci: Debug build"
  cmake --build --preset "$preset-debug" || exit 4
  echo "=== verify ci: Debug tests, quick tier"
  ctest --preset "$preset-debug-quick" --parallel "$jobs" || exit 5
fi
echo "=== verify $mode: Release build"
cmake --build --preset "$preset-release" || exit 6
if [ "$mode" = quick ]; then
  echo "=== verify quick: Release tests, quick tier"
  ctest --preset "$preset-release-quick" --parallel "$jobs" || exit 7
else
  echo "=== verify $mode: Release tests, every tier"
  ctest --preset "$preset-release" --parallel "$jobs" || exit 7
fi
echo "=== verify $mode: passed"
