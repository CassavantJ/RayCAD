"""S9 summary: reads <run_dir>/results.jsonl and targets.jsonl, prints markdown tables.
usage: python summarize.py <run_dir>"""
import json, os, statistics, sys
from collections import Counter, defaultdict

run = sys.argv[1]
R = [json.loads(l) for l in open(os.path.join(run, "results.jsonl"))]
T = [json.loads(l) for l in open(os.path.join(run, "targets.jsonl"))]
R = [r for r in R if r["category"] != "no_target"]
CATS = ["ok", "api_error", "exception", "invalid", "no_op", "wrong", "timeout", "crash", "hard_crash"]
ORDER = ["del_fillet", "del_fillet_chain", "del_hole", "del_hole_stack", "del_pocket",
         "rs_cut_grow", "rs_defeat_grow", "rs_defeat_shrink", "rs_plug_shrink",
         "rs_off_ip_grow", "rs_off_ip_shrink", "rs_off_ic_grow", "rs_off_ic_shrink",
         "rs_off_ap_grow", "rs_off_ap_shrink",
         "pp_prism_out", "pp_prism_in", "pp_prismu_out", "pp_prismu_in", "pp_off_ip_out", "pp_off_ip_in",
         "pp_off_ic_out", "pp_off_ic_in", "pp_off_ap_out", "pp_off_ap_in"]


def real(r):
    return r["file"].startswith("nist")


def pct(a, b):
    return "-" if b == 0 else "%.0f%%" % (100.0 * a / b)


def table(rows, title):
    print("\n### " + title + "\n")
    print("| edit | attempts | solids | ok | rate | " + " | ".join(CATS[1:]) + " | median ms | worst ms |")
    print("|-|-|-|-|-|" + "-|" * (len(CATS) - 1) + "-|-|")
    by = defaultdict(list)
    for r in rows:
        by[r["edit"]].append(r)
    for e in ORDER:
        rs = by.get(e, [])
        if not rs:
            continue
        c = Counter(r["category"] for r in rs)
        ts = [10000.0 if r["category"] == "timeout" else r["t_ms"] for r in rs if r.get("t_ms", -1) >= 0]
        print("| %s | %d | %d | %d | %s | %s | %s | %s |" % (
            e, len(rs), len(set(r["file"] for r in rs)), c["ok"], pct(c["ok"], len(rs)),
            " | ".join(str(c[k]) for k in CATS[1:]),
            "%.0f" % statistics.median(ts) if ts else "-", "%.0f" % max(ts) if ts else "-"))
    other = set(r["category"] for r in rows) - set(CATS)
    if other:
        print("\nother categories:", other)


print("attempts total:", len(R), " files:", len(set(r["file"] for r in R)))
print("category totals:", dict(Counter(r["category"] for r in R)))
table(R, "All 50 solids")
table([r for r in R if real(r)], "Real parts only (11 NIST solids)")
table([r for r in R if not real(r)], "Synthetic parts only (39 solids)")

# best-of approaches per target
def bestof(name, edits):
    by = defaultdict(dict)
    for r in R:
        if r["edit"] in edits:
            by[(r["file"], r["k"])][r["edit"]] = r["category"] == "ok"
    n = len(by)
    ok = sum(1 for v in by.values() if any(v.values()))
    okr = sum(1 for k, v in by.items() if k[0].startswith("nist") and any(v.values()))
    nr = sum(1 for k in by if k[0].startswith("nist"))
    print("| %s | %d | %d | %s | %s | %s |" % (name, n, ok, pct(ok, n), pct(okr, nr), pct(ok - okr, n - nr)))


print("\n### Best of several approaches on the same target\n")
print("| combination | targets | ok | rate | real | synthetic |\n|-|-|-|-|-|-|")
bestof("resize grow: cut, else defeature+cut", ["rs_cut_grow", "rs_defeat_grow"])
bestof("resize grow: any approach", ["rs_cut_grow", "rs_defeat_grow", "rs_off_ip_grow", "rs_off_ic_grow", "rs_off_ap_grow"])
bestof("resize shrink: defeature+cut, else offset", ["rs_defeat_shrink", "rs_off_ip_shrink", "rs_off_ic_shrink", "rs_off_ap_shrink"])
bestof("resize shrink: defeature+cut only", ["rs_defeat_shrink"])
bestof("resize shrink: sleeve fuse, else defeature+cut", ["rs_plug_shrink", "rs_defeat_shrink"])
bestof("push/pull out: prism+unify, else prism without unify", ["pp_prismu_out", "pp_prism_out"])
bestof("push/pull in: prism+unify, else prism without unify", ["pp_prismu_in", "pp_prism_in"])
bestof("push/pull out: offset(ip), else prism", ["pp_off_ip_out", "pp_prism_out"])
bestof("push/pull in: offset(ip), else prism", ["pp_off_ip_in", "pp_prism_in"])
bestof("push/pull out: any", ["pp_off_ip_out", "pp_off_ic_out", "pp_off_ap_out", "pp_prism_out", "pp_prismu_out"])
bestof("push/pull in: any", ["pp_off_ip_in", "pp_off_ic_in", "pp_off_ap_in", "pp_prism_in", "pp_prismu_in"])

print("\n### Push/pull by kind of neighbour faces\n")
print("Square = every neighbour meets the target at 90 degrees (prism+boolean then equals a true face offset).")
print("Other = at least one neighbour is tangent (fillet) or at another angle (chamfer, draft); there a prism leaves a step or notch.\n")
print("| edit | square: attempts | square: ok | other: attempts | other: ok |\n|-|-|-|-|-|")
for e in ORDER:
    if not e.startswith("pp_"):
        continue
    rs = [r for r in R if r["edit"] == e and "nonperp" in r]
    sq = [r for r in rs if r["nonperp"] == 0 and r["tangent"] == 0]
    ot = [r for r in rs if not (r["nonperp"] == 0 and r["tangent"] == 0)]
    okc = lambda x: sum(1 for r in x if r["category"] == "ok")
    print("| %s | %d | %d (%s) | %d | %d (%s) |" % (e, len(sq), okc(sq), pct(okc(sq), len(sq)), len(ot), okc(ot), pct(okc(ot), len(ot))))

print("\n### History on successful attempts\n")
print("| edit | ok | history object | no lost faces, no orphan faces | untouched faces: same or Modified | untouched: only Generated | untouched: flagged deleted | untouched: no record | untouched: mapped to a different surface | result faces with no source | target: removed | target: modified | target: neither | replacement tool face traceable |")
print("|-|-|-|-|-|-|-|-|-|-|-|-|-|-|")
by = defaultdict(list)
for r in R:
    if r["category"] == "ok":
        by[r["edit"]].append(r)
for e in ORDER:
    rs = by.get(e, [])
    if not rs:
        continue
    s = lambda k: sum(r.get(k, 0) for r in rs)
    un = s("h_untouched")
    clean = sum(1 for r in rs if r["hist_avail"] and r["h_lost"] == 0 and r["h_orphans"] == 0)
    nt = s("ntarget_faces")
    print("| %s | %d | %s | %s | %s | %s | %s | %s | %d of %d | %d | %s | %s | %s | %s |" % (
        e, len(rs), pct(sum(r["hist_avail"] for r in rs), len(rs)), pct(clean, len(rs)),
        pct(s("h_mapped"), un), pct(s("h_mapped_gen"), un), pct(s("h_deleted"), un), pct(s("h_lost"), un),
        s("h_inconsistent"), un,
        s("h_orphans"), pct(s("h_t_removed"), nt), pct(s("h_t_modified"), nt),
        pct(nt - s("h_t_removed") - s("h_t_modified"), nt),
        ("%d of %d" % (s("h_key_found"), s("h_key"))) if s("h_key") else "n/a"))

print("\n### Failure details (count by edit, category, message)\n")
c = Counter()
for r in R:
    if r["category"] != "ok":
        d = r.get("detail", "")
        if r["category"] == "no_op":
            d = d[:110]
        c[(r["edit"], r["category"], d[:140])] += 1
for (e, cat, d), n in sorted(c.items(), key=lambda x: (ORDER.index(x[0][0]), -x[1])):
    print("- %s / %s x%d: %s" % (e, cat, n, d))

print("\n### Per-solid success by edit (ok/attempts)\n")
groups = [("del_fillet", ["del_fillet"]), ("chain", ["del_fillet_chain"]), ("del_hole", ["del_hole"]),
          ("stack", ["del_hole_stack"]), ("pocket", ["del_pocket"]), ("rs cut grow", ["rs_cut_grow"]),
          ("rs defeat shrink", ["rs_defeat_shrink"]), ("rs sleeve shrink", ["rs_plug_shrink"]), ("pp off ip", ["pp_off_ip_out", "pp_off_ip_in"]),
          ("pp prism", ["pp_prism_out", "pp_prism_in"]), ("pp prism+unify", ["pp_prismu_out", "pp_prismu_in"])]
print("| solid | faces | " + " | ".join(g[0] for g in groups) + " |")
print("|-|-|" + "-|" * len(groups))
faces = {t["file"]: t.get("faces", 0) for t in T}
for f in sorted(faces):
    cells = []
    for _, es in groups:
        rs = [r for r in R if r["file"] == f and r["edit"] in es]
        cells.append("%d/%d" % (sum(r["category"] == "ok" for r in rs), len(rs)) if rs else "")
    print("| %s | %d | %s |" % (f.replace(".stp", ""), faces[f], " | ".join(cells)))

print("\n### Corpus\n")
print("| solid | bytes | faces | plane | cyl | cone | sphere | torus | other | fillets | chains | holes | stacks | pockets | planar |")
print("|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|")
for t in T:
    if "error" in t:
        print("|", t["file"], "| error |")
        continue
    print("| %s | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d |" % (
        t["file"].replace(".stp", ""), t["bytes"], t["faces"], t["plane_faces"], t["cyl_faces"], t["cone_faces"],
        t["sphere_faces"], t["torus_faces"], t["other_faces"], t["fillets"], t["chains"], t["holes"],
        t["stacks"], t["pockets"], t["planars"]))
