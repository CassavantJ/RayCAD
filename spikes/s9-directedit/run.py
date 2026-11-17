"""S9 driver: one process per attempt, so a hang or hard crash cannot take the run down.
usage: python run.py <corpus_dir> <out_dir> [workers]
Needs the OpenCASCADE bin folder on PATH."""
import json, os, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "build", "s9.exe")

ONLY = set(filter(None, os.environ.get("S9_ONLY", "").split(",")))  # optional subset of edits

EDITS = {
    "fillets": ["del_fillet"],
    "chains": ["del_fillet_chain"],
    "holes": ["del_hole", "rs_cut_grow", "rs_defeat_grow", "rs_defeat_shrink", "rs_plug_shrink",
              "rs_off_ip_grow", "rs_off_ip_shrink", "rs_off_ic_grow", "rs_off_ic_shrink",
              "rs_off_ap_grow", "rs_off_ap_shrink"],
    "stacks": ["del_hole_stack"],
    "pockets": ["del_pocket"],
    "planars": ["pp_prism_out", "pp_prism_in", "pp_prismu_out", "pp_prismu_in", "pp_off_ip_out", "pp_off_ip_in",
                "pp_off_ic_out", "pp_off_ic_in", "pp_off_ap_out", "pp_off_ap_in"],
}


def call(args, timeout):
    t0 = time.time()
    try:
        p = subprocess.run([EXE] + args, capture_output=True, text=True, timeout=timeout)
        rc, out = p.returncode, p.stdout
    except subprocess.TimeoutExpired:
        return None, "driver_timeout", time.time() - t0
    line = None
    for l in out.splitlines():
        l = l.strip()
        if l.startswith("{"):
            try:
                line = json.loads(l)
            except ValueError:
                pass
    return line, rc, time.time() - t0


def attempt(job):
    path, edit, k = job
    d, rc, wall = call(["run", path, edit, str(k)], 180)
    if d is None:
        d = {"file": os.path.basename(path), "edit": edit, "k": k, "t_ms": -1,
             "category": "timeout" if rc == "driver_timeout" else "hard_crash",
             "detail": "process exit code %s, no result line" % (rc,)}
    d["wall_s"] = round(wall, 2)
    return d


def main():
    corpus, outdir = sys.argv[1], sys.argv[2]
    workers = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    os.makedirs(outdir, exist_ok=True)
    files = sorted(f for f in os.listdir(corpus) if f.lower().endswith((".stp", ".step")))
    jobs, targets = [], []
    for f in files:
        path = os.path.join(corpus, f)
        d, rc, _ = call(["targets", path], 300)
        if d is None or "error" in d:
            print("SKIP", f, d, rc)
            targets.append({"file": f, "error": str(d or rc)})
            continue
        d["bytes"] = os.path.getsize(path)
        targets.append(d)
        for key, edits in EDITS.items():
            for e in edits:
                if ONLY and e not in ONLY:
                    continue
                for k in range(min(5, d[key])):
                    jobs.append((path, e, k))
    with open(os.path.join(outdir, "targets.jsonl"), "w") as fh:
        for t in targets:
            fh.write(json.dumps(t) + "\n")
    print(len(files), "files,", len(jobs), "attempts")
    t0 = time.time()
    with ThreadPoolExecutor(workers) as ex, open(os.path.join(outdir, "results.jsonl"), "w") as fh:
        for i, d in enumerate(ex.map(attempt, jobs)):
            fh.write(json.dumps(d) + "\n")
            if i % 200 == 0:
                print(i, "done", round(time.time() - t0), "s", flush=True)
    print("finished in", round(time.time() - t0), "s")


if __name__ == "__main__":
    main()
