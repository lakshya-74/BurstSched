#!/usr/bin/env python3
"""Print median (and min-max spread) per configuration from a summary CSV."""
import csv
import statistics
import sys
from collections import OrderedDict

path = sys.argv[1] if len(sys.argv) > 1 else "results/summary_wakeup.csv"
want = ["p50_us", "p99_us", "max_us", "rec_p50_us", "rec_p99_us", "cold_us",
        "hog_ops_per_s", "ctx_switches", "jain", "max_wait_ms"]

groups = OrderedDict()
with open(path) as f:
    reader = csv.DictReader(f)
    fields = reader.fieldnames or []
    for row in reader:
        groups.setdefault(row["config"], []).append(row)


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


# only show columns that exist and have at least one number
cols = [c for c in want if c in fields and
        any(num(r.get(c)) is not None for rows in groups.values() for r in rows)]

print(f"{'config':<15}" + "".join(f"{c:>24}" for c in cols))
for name, rows in groups.items():
    out = f"{name:<15}"
    for c in cols:
        vals = [x for x in (num(r.get(c)) for r in rows) if x is not None]
        if not vals:
            out += f"{'NA':>24}"
            continue
        if c == "jain":
            cell = f"{statistics.median(vals):.4f} ({min(vals):.3f}-{max(vals):.3f})"
        else:
            cell = f"{statistics.median(vals):.1f} ({min(vals):.0f}-{max(vals):.0f})"
        out += f"{cell:>24}"
    print(out)
print("\nmedian (min-max) over repetitions")
