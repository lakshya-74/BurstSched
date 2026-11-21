#!/usr/bin/env python3
"""
Warm-recovery vs cold-start latency from phase_change per-wakeup logs
(results/lat_phase_<config>_<rep>.csv).

warm recovery = first RECOV wakeups after each CPU phase, cycles >= 1
cold start    = cycle 0, wakeup 0 (task has no history yet)

Usage: python analysis/phase_recovery.py [results_dir] [recov]
"""
import csv
import glob
import os
import re
import statistics as st
import sys

import numpy as np

d = sys.argv[1] if len(sys.argv) > 1 else "results"
recov = int(sys.argv[2]) if len(sys.argv) > 2 else 10

by_cfg = {}
for f in sorted(glob.glob(os.path.join(d, "lat_phase_*.csv"))):
    m = re.fullmatch(r"lat_phase_(.+)_(\d+)\.csv", os.path.basename(f))
    if m:
        by_cfg.setdefault(m.group(1), []).append(f)

print(f"{'config':<15}{'warm p50 us':>13}{'warm p99 us':>13}{'warm max us':>13}{'cold ms':>10}{'reps':>6}")
for cfg, files in by_cfg.items():
    p50s, p99s, mxs, cold = [], [], [], []
    for f in files:
        rows = [(int(r["cycle"]), int(r["idx"]), int(r["latency_ns"]) / 1e3)
                for r in csv.DictReader(open(f))]
        warm = np.array([l for cy, i, l in rows if cy >= 1 and i < recov])
        if len(warm) == 0:
            continue
        cold += [l for cy, i, l in rows if cy == 0 and i == 0]
        p50s.append(np.percentile(warm, 50))
        p99s.append(np.percentile(warm, 99))
        mxs.append(warm.max())
    if p50s:
        print(f"{cfg:<15}{st.median(p50s):>13.1f}{st.median(p99s):>13.1f}{st.median(mxs):>13.1f}"
              f"{st.median(cold) / 1e3 if cold else float('nan'):>10.1f}{len(p50s):>6}")
print("\nmedian over repetitions")
