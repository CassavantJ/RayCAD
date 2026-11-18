# Spike S11: merge several timing runs into one table.
# Usage: python -I tools/summarize.py results/timing_run1.txt results/timing_run2.txt ...
# For each (size, case, mode, method): median of the per-run medians, range of the per-run medians,
# worst step over all runs, mismatches / fallbacks / failed steps summed over runs, and whether the
# result hash was the same in every run.
import re
import sys
from collections import OrderedDict

head = re.compile(r'^-- rows (\d+) \((\d+) curves\), (D\d) .*?, (FreeCAD default|re-init every step)')
step = re.compile(r'^\s+STEP (.+?)\s+n=(\d+)\s+median\s+([\d.]+) ms\s+p95\s+([\d.]+)\s+worst\s+([\d.]+) \| '
                  r'mismatch>[^:]+: (\d+) \(max ([^)]+)\) \| fallbacks (\d+) \| failed (\d+) \| hash (\w+)')
rows = OrderedDict()
for path in sys.argv[1:]:
    key = None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        m = head.match(line)
        if m:
            key = (int(m.group(2)), m.group(3), 'default' if m.group(4).startswith('FreeCAD') else 're-init')
            continue
        m = step.match(line)
        if m and key:
            k = key + (m.group(1).strip(),)
            r = rows.setdefault(k, dict(med=[], p95=[], worst=[], n=0, mis=0, maxd=0.0, fb=0, fail=0, hashes=set()))
            r['med'].append(float(m.group(3)))
            r['p95'].append(float(m.group(4)))
            r['worst'].append(float(m.group(5)))
            r['n'] += int(m.group(2))
            r['mis'] += int(m.group(6))
            r['maxd'] = max(r['maxd'], float(m.group(7)))
            r['fb'] += int(m.group(8))
            r['fail'] += int(m.group(9))
            r['hashes'].add(m.group(10))


def med(v):
    s = sorted(v)
    return s[len(s) // 2]


print('| Curves | Case | Mode | Method | Steps | Median ms (range of run medians) | p95 ms (max) | Worst ms | '
      'Mismatch > 1e-9 | Max diff | Fallbacks | Failed | Same hash every run |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
for (curves, case, mode, method), r in rows.items():
    print(f"| {curves} | {case} | {mode} | {method} | {r['n']} | {med(r['med']):.3f} "
          f"({min(r['med']):.3f} to {max(r['med']):.3f}) | {max(r['p95']):.3f} | {max(r['worst']):.3f} | "
          f"{r['mis']} | {r['maxd']:.2g} | {r['fb']} | {r['fail']} | {'yes' if len(r['hashes']) == 1 else 'NO'} |")
