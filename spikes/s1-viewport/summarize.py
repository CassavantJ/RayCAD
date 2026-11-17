# Spike S1: turn results/*.jsonl into the tables used in REPORT.md.
# Usage: python summarize.py   (run from spikes/s1-viewport)
import json, statistics, collections, os

def load(name):
    path = os.path.join("results", name)
    if not os.path.exists(path):
        return []
    return [json.loads(l) for l in open(path, encoding="utf-8") if l.strip()]

def med(xs):
    return statistics.median(xs) if xs else float("nan")

main = collections.defaultdict(list)
for r in load("main.jsonl"):
    main[(r["label"], r["glass"])].append(r)
gpu = {(r["label"], r["glass"]): r for r in load("gpu.jsonl")}

print("| Run | Adapter reported by QRhi | Glass | Runs | Median ms | p95 ms (median of runs; min to max) | p99 ms | Mean FPS | FPS at p95 | GPU ms median / p95 | Machine CPU busy % during orbit |")
print("|-|-|-|-|-|-|-|-|-|-|-|")
for (label, glass), rs in main.items():
    o = [r["orbit"] for r in rs]
    busy = [r.get("machineCpuBusyPercentDuringOrbit", float("nan")) for r in rs]
    p95 = [x["p95Ms"] for x in o]
    g = gpu.get((label, glass), {}).get("gpu", {})
    gtxt = f'{g["medianMs"]:.2f} / {g["p95Ms"]:.2f}' if g.get("n") else "n/a"
    dev = rs[0]["device"].split("/")[0].replace("NVIDIA Corporation ", "")
    print(f'| {label} | {dev} ({rs[0]["backend"]}) | {"on" if glass else "off"} | {len(rs)} | {med([x["medianMs"] for x in o]):.2f} | '
          f'{med(p95):.2f} ({min(p95):.2f} to {max(p95):.2f}) | {med([x["p99Ms"] for x in o]):.2f} | '
          f'{med([x["fpsMean"] for x in o]):.0f} | {1000 / med(p95):.0f} | {gtxt} | {min(busy):.0f} to {max(busy):.0f} |')

print()
print("| Run | Glass | First frame ms (process start) | Pick idle median / p95 ms | Pick while rendering median / p95 ms | Picks correct | Resize median / p95 / max ms | Idle frames in 2 s |")
print("|-|-|-|-|-|-|-|-|")
for (label, glass), rs in main.items():
    pk = [r["pick"] for r in rs]
    rz = [r["resize"] for r in rs]
    print(f'| {label} | {"on" if glass else "off"} | {med([r["firstFrameMsFromProcessStart"] for r in rs]):.0f} | '
          f'{med([p["idle"]["medianMs"] for p in pk]):.1f} / {med([p["idle"]["p95Ms"] for p in pk]):.1f} | '
          f'{med([p["whileRendering"]["medianMs"] for p in pk]):.1f} / {med([p["whileRendering"]["p95Ms"] for p in pk]):.1f} | '
          f'{sum(p["correct"] for p in pk)}/{sum(p["picks"] for p in pk)} | '
          f'{med([x["medianMs"] for x in rz]):.2f} / {med([x["p95Ms"] for x in rz]):.2f} / {max(x["maxMs"] for x in rz):.1f} | '
          f'{max(r["idleFramesIn2s"] for r in rs)} |')

print()
print("| Extra run | Adapter | Glass | dpr | Target px | Median ms | p95 ms | Mean FPS | Picks correct |")
print("|-|-|-|-|-|-|-|-|-|")
for r in load("extra.jsonl"):
    o = r["orbit"]
    pk = r.get("pick")
    dev = r["device"].split("/")[0].replace("NVIDIA Corporation ", "")
    print(f'| {r["label"]} | {dev} ({r["backend"]}) | {"on" if r["glass"] else "off"} | {r["dpr"]} | {r["rtSize"]} | '
          f'{o["medianMs"]:.2f} | {o["p95Ms"]:.2f} | {o["fpsMean"]:.0f} | {str(pk["correct"]) + "/" + str(pk["picks"]) if pk else "-"} |')
