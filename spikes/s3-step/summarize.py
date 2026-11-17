#!/usr/bin/env python3
"""Spike S3: turn results/*.txt (raw lines from s3bench) into the markdown tables used in REPORT.md.
Usage: python summarize.py [results-dir]   (prints to stdout)"""
import glob, os, re, statistics, sys
from collections import defaultdict

D = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "results")


def parse(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.rstrip("\n").split("|")
        if len(p) < 4 or p[0] not in ("R", "C"):
            continue
        kv = {}
        for x in p[4:]:
            if "=" in x:
                k, v = x.split("=", 1)
                kv[k] = v
        rows.append((p[0], p[1], p[2], p[3], kv))
    return rows


def med(v):
    return statistics.median(v) if v else float("nan")


def fmt_ms(v):
    if v != v:
        return "n/a"
    return f"{v / 1000:.2f} s" if v >= 1000 else f"{v:.0f} ms"


def stat(v):
    if not v:
        return "n/a"
    if len(v) == 1:
        return f"{fmt_ms(v[0])} (n=1)"
    return f"{fmt_ms(med(v))} (min {fmt_ms(min(v))}, n={len(v)})"


files = {}  # key fN -> name
full = defaultdict(lambda: defaultdict(list))  # fN -> (stage,config) -> [kv per run]
for prefix in ("full", "delabella", "defheal", "nist"):
    for path in sorted(glob.glob(os.path.join(D, prefix + "_*.txt"))):
        m = re.match(prefix + r"_(?:r(\d)_)?(\w+)\.txt", os.path.basename(path))
        if not m:
            continue
        fk = m.group(2)
        for kind, fname, stage, cfg, kv in parse(path):
            files[fk] = fname
            kv["_run"] = m.group(1) or "1"
            full[(prefix, fk)][(stage, cfg)].append(kv)


def vals(prefix, fk, stage, cfg, key="ms"):
    return [float(kv[key]) for kv in full[(prefix, fk)].get((stage, cfg), []) if key in kv]


def first(prefix, fk, stage, cfg, key):
    r = full[(prefix, fk)].get((stage, cfg), [])
    return r[0].get(key, "?") if r else "?"


fkeys = sorted(k for k in files if (("full", k) in full))
tiers = ["low", "med", "high"]
modes = ["st", "occt", "pool", "hybrid"]

for prefix, title in (("full", "synthetic files"), ("nist", "NIST files")):
    keys = sorted(k for (p, k) in full if p == prefix)
    if not keys:
        continue
    print(f"\n### Import stages, {title}\n")
    print("| File | Size | Solids / faces | `ReadFile` | Transfer (no heal) | `ShapeFix_Shape` | Reader release | Peak WS after heal |")
    print("|-|-|-|-|-|-|-|-|")
    for fk in keys:
        print(f"| {fk.upper()} `{files[fk]}` | {float(first(prefix, fk, 'file', '-', 'sizeMB')):.1f} MiB | "
              f"{first(prefix, fk, 'shape', '-', 'solids')} / {first(prefix, fk, 'shape', '-', 'faces')} | "
              f"{stat(vals(prefix, fk, 'read', '-'))} | {stat(vals(prefix, fk, 'transfer', 'no-heal'))} | "
              f"{stat(vals(prefix, fk, 'heal', 'ShapeFix_Shape'))} | {stat(vals(prefix, fk, 'release-reader', '-'))} | "
              f"{med(vals(prefix, fk, 'heal', 'ShapeFix_Shape', 'peakMB')):.0f} MB |")

    print(f"\n### Tessellation (default Watson algorithm), {title}\n")
    print("| File | Tier | Triangles | Single thread | OCCT `InParallel` | Pool, one job per body | Pool + `InParallel` | Best speed-up |")
    print("|-|-|-|-|-|-|-|-|")
    for fk in keys:
        for t in tiers:
            cells, meds = [], {}
            for mo in modes:
                v = vals(prefix, fk, "mesh", f"{t}/{mo}")
                cells.append(stat(v))
                if v:
                    meds[mo] = med(v)
            tris = first(prefix, fk, "mesh", f"{t}/pool", "tris")
            best = ""
            if "st" in meds and len(meds) > 1:
                bm = min((m for m in meds if m != "st"), key=lambda m: meds[m])
                best = f"{meds['st'] / meds[bm]:.1f}x ({bm})"
            print(f"| {fk.upper()} | {t} | {tris} | " + " | ".join(cells) + f" | {best} |")

    print(f"\n### Peak working set after each tessellation, run 1, cumulative within the process, {title}\n")
    print("| File | after import | " + " | ".join(f"{t}/{m}" for t in tiers for m in modes) + " |")
    print("|-|-|" + "-|" * 12)
    for fk in keys:
        r = [kv for kv in full[(prefix, fk)].get(("heal", "ShapeFix_Shape"), []) if kv["_run"] == "1"]
        row = [r[0]["peakMB"] if r else "?"]
        for t in tiers:
            for mo in modes:
                rr = [kv for kv in full[(prefix, fk)].get(("mesh", f"{t}/{mo}"), []) if kv["_run"] == "1"]
                row.append(rr[0]["peakMB"] if rr else "n/a")
        print(f"| {fk.upper()} | " + " | ".join(row) + " |")

    print(f"\n### Run-to-run and mode-to-mode identity, {title}\n")
    print("| File | Runs compared | B-rep box identical | Per tier: triangle count, node count, mesh hash and mesh box identical across runs and modes |")
    print("|-|-|-|-|")
    for fk in keys:
        boxes = {kv.get("brepBox") for kv in full[(prefix, fk)].get(("shape", "-"), [])}
        nruns = len(full[(prefix, fk)].get(("shape", "-"), []))
        res = []
        for t in tiers:
            sigs = set()
            n = 0
            for mo in modes:
                for kv in full[(prefix, fk)].get(("mesh", f"{t}/{mo}"), []):
                    sigs.add((kv["tris"], kv["nodes"], kv["hash"], kv["meshBox"]))
                    n += 1
            res.append(f"{t}: {'yes' if len(sigs) == 1 else 'NO (' + str(len(sigs)) + ' variants)'} ({n} meshes)")
        print(f"| {fk.upper()} | {nruns} | {'yes' if len(boxes) == 1 else 'NO'} | " + "; ".join(res) + " |")

# default heal
if any(p == "defheal" for (p, k) in full):
    print("\n### Transfer with OCCT's built-in `FixShape` (reader default) against transfer without it plus explicit `ShapeFix_Shape`\n")
    print("| File | Transfer, built-in FixShape (n=1) | Transfer no-heal + ShapeFix_Shape (medians) | Low-tier triangles equal to explicit path |")
    print("|-|-|-|-|")
    for fk in fkeys:
        a = vals("defheal", fk, "transfer", "default-heal")
        b = med(vals("full", fk, "transfer", "no-heal")) + med(vals("full", fk, "heal", "ShapeFix_Shape"))
        same = first("defheal", fk, "mesh", "low/pool", "tris") == first("full", fk, "mesh", "low/pool", "tris")
        print(f"| {fk.upper()} | {stat(a)} | {fmt_ms(b)} | {'yes' if same else 'no'} |")

# delabella
if any(p == "delabella" for (p, k) in full):
    print("\n### Tessellation with `IMeshTools_MeshAlgoType_Delabella`\n")
    print("| File | Tier | Triangles (same as Watson?) | Single thread | OCCT `InParallel` | Pool per body | Watson pool / Delabella pool | Peak WS after tier (run 1) | Identical across runs and modes |")
    print("|-|-|-|-|-|-|-|-|-|")
    for fk in fkeys:
        for t in tiers:
            sigs = set()
            for mo in modes:
                for kv in full[("delabella", fk)].get(("mesh", f"{t}/{mo}"), []):
                    sigs.add((kv["tris"], kv["nodes"], kv["hash"], kv["meshBox"]))
            tr = first("delabella", fk, "mesh", f"{t}/pool", "tris")
            same = "yes" if tr == first("full", fk, "mesh", f"{t}/pool", "tris") else "no"
            wp, dp = med(vals("full", fk, "mesh", f"{t}/pool")), med(vals("delabella", fk, "mesh", f"{t}/pool"))
            r1 = [kv for kv in full[("delabella", fk)].get(("mesh", f"{t}/pool"), []) if kv["_run"] == "1"]
            print(f"| {fk.upper()} | {t} | {tr} ({same}) | {stat(vals('delabella', fk, 'mesh', f'{t}/st'))} | "
                  f"{stat(vals('delabella', fk, 'mesh', f'{t}/occt'))} | {stat(vals('delabella', fk, 'mesh', f'{t}/pool'))} | "
                  f"{wp / dp:.1f}x | {r1[0]['peakMB'] if r1 else '?'} MB | {'yes' if len(sigs) == 1 else 'NO'} |")

# sweep
sw = sorted(glob.glob(os.path.join(D, "sweep_*.txt")))
if sw:
    print("\n### Thread-count sweep, largest file, Medium tolerance\n")
    print("| Threads | Pool per body | OCCT `InParallel` |")
    print("|-|-|-|")
    data = defaultdict(dict)
    for path in sw:
        m = re.match(r"sweep_(pool|occt)_t(\d+)\.txt", os.path.basename(path))
        for kind, fname, stage, cfg, kv in parse(path):
            if stage == "mesh":
                data[int(m.group(2))][m.group(1)] = float(kv["ms"])
    for n in sorted(data):
        print(f"| {n} | {fmt_ms(data[n].get('pool', float('nan')))} | {fmt_ms(data[n].get('occt', float('nan')))} |")

# cancel
cf = sorted(glob.glob(os.path.join(D, "cancel_*.txt")))
if cf:
    print("\n### Cancel latency\n")
    print("| File | Stage | Uncancelled time | Longest gap between `UserBreak` polls | Latency of each cancel request (requested at fraction of stage) |")
    print("|-|-|-|-|-|")
    for path in cf:
        fk = re.match(r"cancel_(\w+)\.txt", os.path.basename(path)).group(1)
        base, order, trials = {}, [], defaultdict(list)
        for kind, fname, stage, cfg, kv in parse(path):
            if cfg.endswith(" cancel"):
                key = (stage, cfg[:-7])
                lat = float(kv["latencyMs"])
                extra = f", {kv['phase'].split('(')[0]}" if "phase" in kv else ""
                trials[key].append(("missed" if lat < 0 else fmt_ms(lat)) + f" (at {kv['at']}{extra})")
            else:
                key = (stage, cfg)
                base[key] = kv
            if key not in order:
                order.append(key)
        for key in order:
            kv = base.get(key, {})
            b = kv.get("baselineMs") or kv.get("ms") or kv.get("returnedAfterMs")
            if key[1] == "ReadStream" and ("read", "ReadStream uncancelled") in base:
                b = base[("read", "ReadStream uncancelled")]["ms"]
            gap = kv.get("maxPollGapMs")
            note = ""
            if key[1] == "ReadStream uncancelled":
                note = f"stream fully consumed at {fmt_ms(float(kv['streamConsumedAtMs']))}; {fmt_ms(float(kv['afterStreamMs']))} more with no stream access"
            if key[1] == "DESTEP_Provider precancelled":
                note = f"cancel requested before the call; returned after {fmt_ms(float(kv['returnedAfterMs']))} with {kv['polls']} poll(s)"
            if key[1] == "ReadFile":
                note = "no progress argument; cannot be requested"
            print(f"| {fk.upper()} | {key[0]} {key[1]} | {fmt_ms(float(b)) if b else 'n/a'} | "
                  f"{fmt_ms(float(gap)) if gap else 'n/a'} | {note}{'; '.join(trials.get(key, []))} |")

# cancel, condensed: worst observed latency per stage and file
if cf:
    print("\n### Cancel latency, worst case per stage\n")
    cols, cell = [], defaultdict(dict)
    for path in cf:
        fk = re.match(r"cancel_(\w+)\.txt", os.path.basename(path)).group(1)
        if fk.startswith("delabella"):
            continue
        cols.append(fk)
        worst, gaps, base = defaultdict(float), defaultdict(float), {}
        for kind, fname, stage, cfg, kv in parse(path):
            c = cfg.replace(" cancel", "")
            if stage == "read":
                if cfg == "ReadStream uncancelled":
                    base["total"], base["eof"] = float(kv["ms"]), float(kv["streamConsumedAtMs"])
                elif cfg == "ReadStream cancel":
                    k = "read, parse phase (stream cut)" if kv["phase"] == "parse" else "read, load phase"
                    worst[k] = max(worst[k], float(kv["latencyMs"]))
                continue
            k = f"{stage} {c}" if stage != "mesh" else f"mesh {c.split('/')[0]} ({c.split('/')[1]})"
            if "latencyMs" in kv:
                worst[k] = max(worst[k], float(kv["latencyMs"]))
            if "maxPollGapMs" in kv:
                gaps[k] = max(gaps[k], float(kv["maxPollGapMs"]))
        for k in set(worst) | set(gaps):
            cell[k][fk] = f"{fmt_ms(worst[k])}" + (f" / {fmt_ms(gaps[k])}" if k in gaps else "")
        if base:
            cell["read, load phase"][fk] = f"{fmt_ms(worst['read, load phase'])} / {fmt_ms(base['total'] - base['eof'])}"
    order = ["read, parse phase (stream cut)", "read, load phase", "transfer no-heal", "transfer default-heal", "heal ShapeFix_Shape"]
    order += [f"mesh {t} ({m})" for t in tiers for m in ("st", "occt", "pool")]
    print("| Stage | " + " | ".join(c.upper() for c in cols) + " |")
    print("|-|" + "-|" * len(cols))
    for k in order:
        if k in cell:
            print(f"| {k} | " + " | ".join(cell[k].get(c, "not run") for c in cols) + " |")
