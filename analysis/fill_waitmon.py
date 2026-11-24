#!/usr/bin/env python3
"""
Fill the wm_* columns of summary_<workload>_full.csv from the waitmon logs
(results/wait_<workload>_full_<config>_<rep>.txt). Needed for runs made before
quick_compare.sh parsed the right waitmon line.

Usage: python analysis/fill_waitmon.py [results_dir]
"""
import csv
import glob
import os
import re
import sys

d = sys.argv[1] if len(sys.argv) > 1 else "results"
KEYS = {"wm_max_ms": "max_all_ms", "wm_max_hog_ms": "max_hog_ms",
        "wm_max_work_ms": "max_work_ms", "wm_p99_hog_ms": "p99_hog_ms"}

for path in sorted(glob.glob(os.path.join(d, "summary_*_full.csv"))):
    tag = os.path.basename(path)[len("summary_"):-len(".csv")]
    with open(path) as f:
        rd = csv.DictReader(f)
        fields, rows = rd.fieldnames, list(rd)
    filled = 0
    for r in rows:
        wfile = os.path.join(d, f"wait_{tag}_{r['config']}_{r['rep']}.txt")
        if not os.path.exists(wfile):
            continue
        line = next((l for l in open(wfile) if l.startswith("waitmon: max_all_ms=")), "")
        for col, key in KEYS.items():
            m = re.search(rf"\b{key}=([0-9.]+)", line)
            if m:
                r[col] = m.group(1)
        filled += bool(line)
    with open(path, "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
        wr.writeheader()
        wr.writerows(rows)
    print(f"{os.path.basename(path)}: filled waitmon values for {filled}/{len(rows)} runs")
