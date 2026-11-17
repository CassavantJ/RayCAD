#!/usr/bin/env bash
# S5 spike: run every case on both Adaptive variants, check, and test determinism. Run from spikes/s5-adaptive.
set -u
B=build; O=out; mkdir -p $O
CASES="p1 p2 p3 p4 p5 p6 p4b p4c p4d p4e p4f"
for so in 0.2 0.1; do
  for v in main 114; do
    for c in $CASES; do
      $B/s5_gen_$v run $c $so $O/${c}_${v}_$so.tp 5 2>&1
      $B/s5_check $c $O/${c}_${v}_$so.tp $so $O/${c}_${v}_$so.png
    done > $O/results_${v}_$so.txt 2>&1
  done
done
# determinism: 20 fresh processes per case, byte compare of the toolpath file
for v in main 114; do
  for c in p1 p2 p3 p4 p4d p5 p6; do
    for i in $(seq 1 20); do $B/s5_gen_$v run $c 0.2 $O/det_tmp.tp 1 >/dev/null 2>&1; sha256sum < $O/det_tmp.tp; done | sort | uniq -c | sed "s/^/proc20 $v $c /"
  done
done > $O/determinism.txt 2>&1

for v in main 114; do
  echo "== $v in-process 20 reps"; $B/s5_gen_$v det 0.2 20 2>/dev/null
  for t in 2 8 32; do echo "== $v concurrent threads=$t reps=3"; $B/s5_gen_$v mt 0.2 $t 3 2>/dev/null; done
  echo "== $v cancel"; $B/s5_gen_$v cancel 0.2 2>/dev/null
done >> $O/determinism.txt 2>&1
