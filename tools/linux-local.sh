#!/usr/bin/env bash
# Build and test RayCAD on Linux the way CI does, from a Windows checkout through WSL.
#
#   wsl.exe -d raycad-build -u root -- bash /mnt/m/RayCAD/tools/linux-local.sh [preset] [config ...]
#
# preset: linux-gcc (default), linux-clang-asan or linux-clang-tsan. config: debug and/or release
# (default: debug). The source tree is copied into the distribution's own file system first, because
# building on a Windows drive through /mnt is slow. vcpkg, its binary cache and Qt live under $HOME
# and are reused. RAYCAD_WORK (default $HOME/raycad) is the folder the source is copied to and built
# in; give each concurrent build its own, for example: wsl.exe ... -- env RAYCAD_WORK=/tmp/w bash ...
set -euo pipefail

src="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="${1:-linux-gcc}"
shift || true
configs=("$@")
[ ${#configs[@]} -eq 0 ] && configs=(debug)

work="${RAYCAD_WORK:-$HOME/raycad}"
mkdir -p "$work"
# Files are compared by content, and a copied file gets the time of the copy, not its time in the checkout
# (--no-times). With -a's preserved times, a source that changed in the checkout while a build of the previous copy
# was running (a merge, a checkout) could arrive older than the objects built meanwhile from its old content; ninja
# then kept those stale objects and linked them with code built from the new headers, which corrupted memory at
# run time (a GCC debug ray_doc_tests aborted with "stack smashing detected" until the object was rebuilt).
rsync -a --no-times --checksum --delete --exclude .git --exclude build --exclude 'spikes/*/build*' \
      --exclude 'spikes/*/third_party' --exclude 'spikes/*/data' "$src/" "$work/src/"
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
# Every requested configuration is built and tested even if an earlier one failed; the exit code says whether any did.
summary=()
failed=0
for c in "${configs[@]}"; do
  if cmake --list-presets=build | grep -q "\"$preset-$c\""; then b="$preset-$c"; else b="$preset"; fi
  echo "== build $b"
  if ! cmake --build --preset "$b"; then
    summary+=("$b: build FAILED")
    failed=1
    continue
  fi
  echo "== test $b"
  if ctest --preset "$b" --parallel "$(nproc)"; then
    summary+=("$b: tests passed")
  else
    summary+=("$b: tests FAILED")
    failed=1
  fi
done
echo "== linux-local summary"
for line in "${summary[@]}"; do echo "   $line"; done
if [ "$failed" -ne 0 ]; then
  echo "== linux-local: FAILED"
  exit 1
fi
echo "== linux-local: all done"
