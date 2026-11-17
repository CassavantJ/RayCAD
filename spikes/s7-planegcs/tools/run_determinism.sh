#!/usr/bin/env bash
# Determinism runs. Run from spikes/s7-planegcs. Jobs run in parallel; timing is not measured here.
set -u
B=./build/s7_bench.exe
for mode in 1 2 4; do
  $B --rows 1 --runs 100 --det-steps 100 --det-mode $mode --no-timing --no-diag > results/det_050_mode$mode.txt 2>&1 &
  $B --rows 4 --runs 100 --det-steps 3 --det-mode $mode --no-timing --no-diag > results/det_200_mode$mode.txt 2>&1 &
done
# Thread and build settings: reference hash only.
{
  for rows in 1 4; do
    steps=100; [ $rows = 4 ] && steps=3
    A="--rows $rows --hash-only --det-steps $steps"
    echo "## rows $rows default build";            $B $A | grep -E 'hash|OpenMP'
    echo "## rows $rows default build, 1 CPU";     cmd //c "start /affinity 1 /wait /b build\s7_bench.exe $A" | grep -E 'hash'
    echo "## rows $rows default build, debugMode Minimal"; $B $A --debug-minimal | grep -E 'hash'
    for t in 1 2 8 24; do
      echo "## rows $rows OpenMP build, Eigen::setNbThreads($t)"; ./build-openmp/s7_bench.exe $A --eigen-threads $t | grep -E 'hash|OpenMP|nbThreads'
    done
    echo "## rows $rows OpenMP build, EIGEN threads untouched (GCS sets 1)"; ./build-openmp/s7_bench.exe $A | grep -E 'hash'
    echo "## rows $rows AVX2 build";               ./build-avx2/s7_bench.exe $A | grep -E 'hash|SIMD'
  done
} > results/det_settings.txt 2>&1
wait
echo done > results/det_done.txt
