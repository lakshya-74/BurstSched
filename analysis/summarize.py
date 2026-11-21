#!/usr/bin/env python3
"""Print median (and min-max spread) per configuration from results/summary.csv."""
import csv
import statistics
import sys
from collections import OrderedDict

path = sys.argv[1] if len(sys.argv) > 1 else "results/summary.csv"
cols = ["p50_us", "p99_us", "max_us", "hog_ops_per_s", "ctx_switches", "max_wait_ms"]

groups = OrderedDict()
with open(path) as f:
    for row in csv.DictReader(f):
        groups.setdefault(row["config"], []).append(row)


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


print(f"{'config':<15}" + "".join(f"{c:>22}" for c in cols))
for name, rows in groups.items():
    out = f"{name:<15}"
    for c in cols:
        vals = [x for x in (num(r[c]) for r in rows) if x is not None]
        if not vals:
            out += f"{'NA':>22}"
            continue
        med = statistics.median(vals)
        cell = f"{med:.1f} ({min(vals):.0f}-{max(vals):.0f})"
        out += f"{cell:>22}"
    print(out)
print("\nmedian (min-max) over repetitions")
