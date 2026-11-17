#!/usr/bin/env python3
"""Spike S3: fill report_body.in with the tables printed by summarize.py and write REPORT.md.
A template line of the form  @@TABLE <heading>@@  is replaced by the table under '### <heading>'."""
import os, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
out = subprocess.run([sys.executable, os.path.join(here, "summarize.py")], capture_output=True, text=True, encoding="utf-8").stdout
tables, cur = {}, None
for line in out.splitlines():
    if line.startswith("### "):
        cur = line[4:].strip()
        tables[cur] = []
    elif cur is not None and line.strip():
        tables[cur].append(line)
res = []
for line in open(os.path.join(here, "report_body.in"), encoding="utf-8").read().splitlines():
    if line.startswith("@@TABLE ") and line.endswith("@@"):
        name = line[8:-2].strip()
        if name not in tables:
            sys.exit(f"no table named: {name}")
        res.extend(tables[name])
    else:
        res.append(line)
open(os.path.join(here, "REPORT.md"), "w", encoding="utf-8", newline="\n").write("\n".join(res) + "\n")
print("REPORT.md written,", len(res), "lines")
