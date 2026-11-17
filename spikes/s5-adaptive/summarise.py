# S5 spike: turn out/results_<variant>_<stepover>.txt into markdown tables. Usage: python -I summarise.py
import re, os, sys

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "out")
ORDER = ["p1", "p2", "p3", "p4", "p5", "p6", "p4b", "p4c", "p4d", "p4e", "p4f", "p7"]


def parse(path):
    res, cur = {}, None
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip()
        kv = dict(re.findall(r"([A-Za-z_0-9.<>()%+]+)=(\S+)", line))
        if line.startswith("case=") and "hash=" in line:
            cur = res.setdefault(kv["case"], {})
            cur.update(kv)
        elif line.startswith("case="):
            cur = res.setdefault(kv["case"], {})
            cur.update(kv)
        elif line.startswith("  eng_cut:") and cur is not None:
            cur.update({"c_" + k: v for k, v in kv.items()})
        elif line.startswith("  eng_link_down:") and cur is not None:
            cur.update({"l_" + k: v for k, v in kv.items()})
        elif line.startswith("  hist_cut_pct_by_10deg:") and cur is not None:
            cur["hist"] = line.split(":")[1].split()
        elif line.startswith("  flags") and cur is not None:
            cur["flags"] = line.split("=")[-1]
        elif cur is not None and line.startswith("  "):
            if "uncut_reachable=" in line:
                m = re.search(r"uncut_reachable=(\S+) \((\S+)% of reachable\)", line)
                cur["uncut_reach"], cur["uncut_reach_pct"] = m.group(1), m.group(2)
            cur.update(kv)
        elif "Start point not found" in line:
            pass
    return res


for so in ("0.2", "0.1"):
    for v in ("main", "114"):
        p = os.path.join(out, f"results_{v}_{so}.txt")
        if not os.path.exists(p):
            continue
        r = parse(p)
        print(f"\n### variant={v} stepover={so}\n")
        print("| case | pts | time ms (median of 5) | cut mm | link-down mm | retract-link mm | helix mm | eng median | p95 | p99 | max | % > target | % > 1.1x | % > 1.5x | % > t+5deg | % > t+10deg | % > t+20deg | % air | uncut reachable mm2 (%) | max thick mm | gouge analytic mm | gouge area mm2 | flags |")
        print("|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|")
        for c in ORDER:
            if c not in r:
                continue
            d = r[c]
            g = lambda k: d.get(k, "?")
            print(f"| {c} | {g('points')} | {g('ms_median')} | {g('cut_len')} | {g('link_down_len')} | {g('rapid_len')} | {g('helix_len')} | "
                  f"{g('c_median')} | {g('c_p95')} | {g('c_p99')} | {g('c_max')} | {g('c_pct_above_target')} | {g('c_pct_above_1.1x')} | {g('c_pct_above_1.5x')} | {g('c_pct_above_t+5deg')} | {g('c_pct_above_t+10deg')} | {g('c_pct_above_t+20deg')} | {g('c_pct_air(<2deg)')} | "
                  f"{g('uncut_reach')} ({g('uncut_reach_pct')}) | {g('uncut_reachable_max_thickness')} | {g('gouge_depth_analytic')} | {g('gouge_area')} | {g('flags')} |")
        print("\nHistogram, % of cut length per 10 degree bin (0-10, 10-20, ... 170-180):\n")
        for c in ORDER:
            if c in r and "hist" in r[c]:
                print(f"- {c}: " + " ".join(r[c]["hist"]))
        print("\nLink-down moves (tool at depth over cleared area): max engagement / % length engaged / material removed mm2\n")
        for c in ORDER:
            if c in r:
                d = r[c]
                print(f"- {c}: {d.get('l_max')} deg / {d.get('l_pct_len_engaged(>2deg)')} % / {d.get('l_material_removed_mm2')}; implicit joins {d.get('implicit_joins')} (total {d.get('join_len')} mm, max {d.get('join_max')} mm)")
