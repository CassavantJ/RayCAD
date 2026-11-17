#!/usr/bin/env bash
# Timing runs, one after another, nothing else running. Run from spikes/s7-planegcs.
set -u
B=./build/s7_bench.exe
$B --rows 4 --runs 100 --drag-reps 1 --stateless-steps 5 --no-determinism > results/timing_200.txt 2>&1
$B --rows 2 --runs 30 --drag-reps 1 --stateless-steps 5 --no-determinism --no-diag > results/timing_100.txt 2>&1
$B --rows 1 --runs 100 --drag-reps 3 --stateless-steps 20 --no-determinism --no-diag > results/timing_050.txt 2>&1
$B --rows 3 --runs 30 --drag-reps 1 --stateless-steps 5 --no-determinism --no-diag > results/timing_150.txt 2>&1
./build-avx2/s7_bench.exe --rows 4 --runs 20 --drag-reps 1 --stateless-steps 3 --no-determinism --no-diag --variants 1 > results/timing_200_avx2.txt 2>&1
for opt in "--gauss 1" "--gauss 2" "--alg 1" "--alg 0" "--qr dense" "--debug-minimal"; do
  $B --rows 4 --runs 10 --no-drag --no-determinism --no-diag --variants 2 $opt
done > results/timing_200_options.txt 2>&1
echo done > results/timing_done.txt
