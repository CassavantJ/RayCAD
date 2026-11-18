#!/usr/bin/env bash
# Build and test RayCAD on Linux the way CI does, from a Windows checkout through WSL.
#
#   wsl.exe -d raycad-build -u root -- bash /mnt/m/RayCAD/tools/linux-local.sh [preset] [config ...]
#
# preset: linux-gcc (default) or linux-clang-asan. config: debug and/or release (default: debug).
# The source tree is copied into the distribution's own file system first, because building on a
# Windows drive through /mnt is slow. vcpkg, its binary cache and Qt live under $HOME and are reused.
set -euo pipefail

src="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="${1:-linux-gcc}"
shift || true
configs=("$@")
[ ${#configs[@]} -eq 0 ] && configs=(debug)

work="$HOME/raycad"
mkdir -p "$work"
rsync -a --delete --exclude .git --exclude build --exclude 'spikes/*/build*' --exclude 'spikes/*/third_party' \
      --exclude 'spikes/*/data' "$src/" "$work/src/"
cd "$work/src"

baseline=$(python3 -c "import json; print(json.load(open('vcpkg-configuration.json'))['default-registry']['baseline'])")
export VCPKG_ROOT="$HOME/vcpkg"
export VCPKG_DEFAULT_BINARY_CACHE="$HOME/vcpkg-cache"
mkdir -p "$VCPKG_DEFAULT_BINARY_CACHE"
if [ ! -d "$VCPKG_ROOT/.git" ]; then
  mkdir -p "$VCPKG_ROOT"
  git -C "$VCPKG_ROOT" init -q
  git -C "$VCPKG_ROOT" remote add origin https://github.com/microsoft/vcpkg.git
fi
if [ "$(git -C "$VCPKG_ROOT" rev-parse HEAD 2>/dev/null || true)" != "$baseline" ]; then
  git -C "$VCPKG_ROOT" fetch -q --depth 1 origin "$baseline"
  git -C "$VCPKG_ROOT" checkout -q FETCH_HEAD
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics >/dev/null
fi

QT_ROOT_DIR="$(python3 tools/fetch_qt.py --out "$HOME/qt" --module qtshadertools | tail -1)"
export QT_ROOT_DIR
export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software

echo "== configure $preset"
cmake --preset "$preset"
for c in "${configs[@]}"; do
  if cmake --list-presets=build | grep -q "\"$preset-$c\""; then b="$preset-$c"; else b="$preset"; fi
  echo "== build $b"
  cmake --build --preset "$b"
  echo "== test $b"
  ctest --preset "$b"
done
echo "== linux-local: all done"
