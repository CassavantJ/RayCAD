#!/bin/sh
# Spike S1 measurement matrix. Usage: ./measure.sh main | gpu | extra
# Appends one JSON line per run to results/<set>.jsonl. Each run opens one window for under 20 seconds.
cd "$(dirname "$0")"
mkdir -p results
run() {  # label, variant, glass, output file, extra args...; adapter env comes from the caller
    label=$1; variant=$2; glass=$3; out=$4; shift 4
    ./run.sh "$variant" --glass "$glass" --label "$label" --out "results/$out" "$@" >/dev/null 2>>results/stderr.log
    echo "$label glass=$glass exit=$?"
}
matrix() {  # output file, extra args...
    out=$1; shift
    for glass in 1 0; do
        QT_D3D_ADAPTER_INDEX=0 run A-d3d11-rtx3090 a $glass "$out" "$@"
        QT_D3D_ADAPTER_INDEX=1 run A-d3d11-uhd770 a $glass "$out" "$@"
        QSG_RHI_PREFER_SOFTWARE_RENDERER=1 run A-d3d11-warp a $glass "$out" "$@"
        QSG_RHI_BACKEND=opengl run A-opengl-rtx3090 a $glass "$out" "$@"
        run B-opengl-rtx3090 b $glass "$out" "$@"
    done
}
case "$1" in
main)  matrix "main.jsonl" --seconds 10 --pick --resize --idle ;;
gpu)   matrix "gpu.jsonl" --seconds 5 --gpu-time ;;
extra)
    QT_D3D_ADAPTER_INDEX=1 run A-d3d11-uhd770-msaa4 a 1 extra.jsonl --seconds 5 --msaa 4
    QT_D3D_ADAPTER_INDEX=0 run A-d3d11-rtx3090-msaa4 a 1 extra.jsonl --seconds 5 --msaa 4
    for glass in 1 0; do
        S1_OCCT_SYSTEM_BUFFER=0 run B-opengl-rtx3090-ownfbo b $glass extra.jsonl --seconds 5
    done
    for sf in 0.6667 1 2; do   # device pixel ratio 1.0, 1.5 and 3.0 on this 150 % display
        QT_SCALE_FACTOR=$sf run "A-d3d11-rtx3090-scale$sf" a 1 extra.jsonl --seconds 2 --pick
        QT_SCALE_FACTOR=$sf run "B-opengl-rtx3090-scale$sf" b 1 extra.jsonl --seconds 2 --pick
    done ;;
*) echo "usage: $0 main|gpu|extra"; exit 1 ;;
esac
