#!/usr/bin/env bash
# Spike S3: supplementary runs on real (non-synthetic) NIST MBE PMI STEP files.
# Download and unpack first (see REPORT.md, "Test files"):
#   data/nist/NIST-PMI-STEP-Files/*.stp
cd "$(dirname "$0")"
B=./build/s3bench.exe
N=data/nist/NIST-PMI-STEP-Files
mkdir -p results
run() { local out=results/$1.txt; shift; echo "== $(date +%H:%M:%S) $*" >&2; "$B" "$@" 2>&1 | grep -E '^(R|C|D)\|' > "$out"; }
for r in 1 2 3; do
  run nist_r${r}_n1 run $N/nist_ftc_09_asme1_ap242-e1.stp
  run nist_r${r}_n2 run $N/nist_stc_09_asme1_ap242-e3.stp
  run nist_r${r}_n3 run "$N/AP203 with PMI/nist_ctc_02_asme1_ap203.stp"
done
run cancel_n1 cancel $N/nist_ftc_09_asme1_ap242-e1.stp --trials 3 --modes st,occt,pool
echo "== done $(date +%H:%M:%S)" >&2
