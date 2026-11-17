#!/bin/sh
# Spike S1 runner. Usage: ./run.sh a|b [program options]. Environment variables select the adapter.
cd "$(dirname "$0")"
export PATH="/m/Qt/6.12.0/msvc2022_64/bin:/m/dev/vcpkg/installed/x64-windows/bin:$PATH"
exe=build/s1$1.exe; shift
exec "$exe" "$@"
