#!/usr/bin/env bash
# Spike S3: generate the five synthetic STEP test files into data/ (git-ignored).
# Run from Git Bash:  bash spikes/s3-step/make_data.sh
# Deterministic: body dimensions are derived from the body index.
# Hole counts per planar face are kept at or below about 830: BRepMesh memory and time on a planar face
# grow steeply with the number of hole wires (see REPORT.md), and larger faces would not fit in RAM
# at the High tolerance with 32 concurrent jobs.
set -e
cd "$(dirname "$0")"
GEN=./build/s3gen.exe
mkdir -p data

# spec = W,D,T,pitch,pockets,freeform,seed
small_body() { # index -> a plate of roughly 2 MB of STEP
  local i=$1
  local W=$((360 + (i * 37) % 100)) D=$((260 + (i * 53) % 80)) T=$((30 + (i * 7) % 20))
  local P=$((14 + i % 4)) K=$((3 + i % 3))
  echo "$W,$D,$T,$P,$K,1,$((1000 + i))"
}
bodies() { local from=$1 to=$2 out=""; for ((i = from; i <= to; i++)); do out="$out $(small_body $i)"; done; echo $out; }

# F1: one large plate, one solid (per-body parallelism cannot help here)
$GEN data/f1_plate_1body.step 700,480,45,15,5,1,11 | tail -1
# F2: small assembly, 5 distinct plates
$GEN data/f2_asm_5body.step $(bodies 1 5) | tail -1
# F3: one large plate (F1-like, about 800 holes per planar face) plus 10 small ones
$GEN data/f3_asm_dominant_11body.step 690,470,50,15,5,1,31 $(bodies 11 20) | tail -1
# F4: 24 distinct plates of similar size
$GEN data/f4_asm_24body.step $(bodies 21 44) | tail -1
# F5: two large plates plus 44 small ones
$GEN data/f5_asm_46body.step 700,480,45,15,5,1,51 680,500,55,15,6,1,52 $(bodies 61 104) | tail -1
ls -la data
