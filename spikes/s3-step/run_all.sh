#!/usr/bin/env bash
# Spike S3: run the whole measurement matrix, one process at a time. Raw output goes to results/.
# Run from Git Bash:  bash spikes/s3-step/run_all.sh
cd "$(dirname "$0")"
B=./build/s3bench.exe
mkdir -p results
F1=data/f1_plate_1body.step
F2=data/f2_asm_5body.step
F3=data/f3_asm_dominant_11body.step
F4=data/f4_asm_24body.step
F5=data/f5_asm_46body.step
run() { # out-name, args...
  local out=results/$1.txt; shift
  echo "== $(date +%H:%M:%S) $*" >&2
  "$B" "$@" 2>&1 | grep -E '^(R|C|D)\|' > "$out"
}

# 1. Full matrix, run 1 (all modes, all tiers)
i=1
for f in $F1 $F2 $F3 $F4 $F5; do run full_r1_f$i run $f; i=$((i + 1)); done

# 2. Runs 2 and 3 for run-to-run comparison. Full for F1..F3; pool mode only for F4, F5 (time).
for r in 2 3; do
  i=1
  for f in $F1 $F2 $F3; do run full_r${r}_f$i run $f; i=$((i + 1)); done
  run full_r${r}_f4 run $F4 --modes pool
  run full_r${r}_f5 run $F5 --modes pool
done

# 3. OCCT's built-in healing during transfer (default STEPControl_Reader behaviour)
i=1
for f in $F1 $F2 $F3 $F4 $F5; do run defheal_f$i run $f --default-heal --tiers low --modes pool; i=$((i + 1)); done

# 4. Delabella triangulation instead of the default Watson
i=1
for f in $F1 $F2 $F3 $F4 $F5; do
  run delabella_r1_f$i run $f --algo delabella --modes st,occt,pool
  run delabella_r2_f$i run $f --algo delabella --modes pool
  run delabella_r3_f$i run $f --algo delabella --modes pool
  i=$((i + 1))
done

# 5. Thread-count sweep on the largest file, Medium tolerance
for n in 2 4 8 16 32; do
  run sweep_pool_t${n} run $F5 --tiers med --modes pool --threads $n
  run sweep_occt_t${n} run $F5 --tiers med --modes occt --occt-threads $n
done

# 6. Cancel latency
run cancel_f1 cancel $F1 --trials 3 --modes st,occt,pool
run cancel_f2 cancel $F2 --trials 3 --modes st,occt,pool
run cancel_f3 cancel $F3 --trials 3 --modes st,occt,pool
run cancel_f4 cancel $F4 --trials 3 --modes occt,pool
run cancel_f5 cancel $F5 --trials 3 --modes occt,pool
echo "== done $(date +%H:%M:%S)" >&2
