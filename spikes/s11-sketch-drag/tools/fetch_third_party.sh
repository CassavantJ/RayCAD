#!/usr/bin/env bash
# Spike S11: put the third-party sources into ../third_party (git-ignored, never committed).
# Same pins as spike S7 (see ../../s7-planegcs/REPORT.md):
#   FreeCAD tag 1.1.4, commit 4fd3bf320d9566a27e60069fc8387448aaa3a094, only src/Mod/Sketcher/App/planegcs + LICENSE
#   Eigen   tag 3.4.0, commit 3147391d946bb4b6c68edd901f2add6ac1f31f8c
#   Boost   1.92.0 release archive, header subset made by S7's tools/boost_subset.py
# Default: copy S7's already fetched tree if it exists (that is what this spike's numbers were built with).
# FETCH=1 forces a fresh download instead.
# Usage (Git Bash or any POSIX shell): spikes/s11-sketch-drag/tools/fetch_third_party.sh [s7_third_party_dir]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
tp="$here/third_party"
s7tp="${1:-$here/../s7-planegcs/third_party}"

if [ -z "${FETCH:-}" ] && [ -f "$s7tp/freecad/src/Mod/Sketcher/App/planegcs/GCS.cpp" ]; then
    mkdir -p "$tp"
    for d in freecad eigen boost-subset; do
        rm -rf "${tp:?}/$d"
        cp -r "$s7tp/$d" "$tp/$d"
        rm -rf "$tp/$d/.git"
    done
    echo "copied S7 third-party tree from $s7tp"
    exit 0
fi

mkdir -p "$tp"
cd "$tp"
rm -rf freecad eigen boost-subset

git clone --filter=blob:none --sparse --depth 1 --branch 1.1.4 https://github.com/FreeCAD/FreeCAD.git freecad
git -C freecad sparse-checkout set --no-cone '/src/Mod/Sketcher/App/planegcs/' '/LICENSE'
test "$(git -C freecad rev-parse HEAD)" = 4fd3bf320d9566a27e60069fc8387448aaa3a094

git clone --depth 1 --branch 3.4.0 https://gitlab.com/libeigen/eigen.git eigen
test "$(git -C eigen rev-parse HEAD)" = 3147391d946bb4b6c68edd901f2add6ac1f31f8c

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -fL -o "$tmp/boost.tar.bz2" https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2
tar -xjf "$tmp/boost.tar.bz2" -C "$tmp" boost_1_92_0/boost
python -I "$here/../s7-planegcs/tools/boost_subset.py" "$tmp/boost_1_92_0" "$tp/boost-subset" \
    boost/graph/adjacency_list.hpp,boost/graph/connected_components.hpp,boost/graph/graph_concepts.hpp,boost/math/constants/constants.hpp \
    mpl,preprocessor,config
echo "fetched into $tp"
