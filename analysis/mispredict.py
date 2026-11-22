#!/usr/bin/env python3
"""
How often does BurstSched place a task in the wrong queue, and what does it cost?

Reads burst logs written by `burstsched -o` (results/bursts_<workload>_full_<config>_<rep>.csv).
For every finished burst it compares the queue level the task was placed in with the
level its real burst length called for (same thresholds as level_for_burst() in the BPF
code) and classifies it as:
  correct   - placed at the right level
  too_low   - placed below its level (short burst stuck behind longer work): costs latency
  too_high  - placed above its level (long burst at high priority): costs others / ctx switches
Cost = run-queue wait (wait_ns) of the task before it ran.

Usage: python analysis/mispredict.py [results_dir] [-o data/final/mispredict.csv]
"""
import argparse
import csv
import glob
import os
import re
from collections import defaultdict

import numpy as np

MSEC = 1_000_000
WORKLOAD_COMMS = ("wakeup_lat", "phase_change", "rr_bench", "rr_server")
ORDER = ["mlfq", "ewma", "ewma_conf", "ewma_conf_slc", "full", "learned", "learned_percpu"]


def ideal_level(b):
    if b < 1 * MSEC:
        return 0
    if b < 4 * MSEC:
        return 1
    if b < 12 * MSEC:
        return 2
    return 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results", nargs="?", default="results")
    ap.add_argument("-o", "--out", default="data/final/mispredict.csv")
    args = ap.parse_args()

    # (workload, config, scope) -> lists
    acc = defaultdict(lambda: {"cls": [], "wait": []})
    for f in sorted(glob.glob(os.path.join(args.results, "bursts_*_full_*.csv"))):
        m = re.fullmatch(r"bursts_(\w+?)_full_(.+)_(\d+)\.csv", os.path.basename(f))
        if not m:
            continue
        wl, cfg = m.group(1), m.group(2)
        with open(f) as fh:
            for r in csv.DictReader(fh):
                b, lvl, w = int(r["burst_ns"]), int(r["level"]), int(r["wait_ns"])
                ideal = ideal_level(b)
                c = 0 if lvl == ideal else (1 if lvl > ideal else 2)
                scopes = ["all"]
                if r["comm"].startswith(WORKLOAD_COMMS):
                    scopes.append("workload")
                for s in scopes:
                    acc[(wl, cfg, s)]["cls"].append(c)
                    acc[(wl, cfg, s)]["wait"].append(w)

    if not acc:
        raise SystemExit(f"no bursts_*_full_*.csv files in {args.results}")

    rows = []
    for (wl, cfg, scope), d in acc.items():
        cls = np.array(d["cls"])
        wait = np.array(d["wait"], dtype=float) / 1000.0  # us
        n = len(cls)

        def wstat(mask, q):
            return float(np.percentile(wait[mask], q)) if mask.any() else float("nan")

        ok, low, high = cls == 0, cls == 1, cls == 2
        rows.append({
            "workload": wl, "config": cfg, "scope": scope, "bursts": n,
            "correct_pct": 100 * ok.mean(), "too_low_pct": 100 * low.mean(),
            "too_high_pct": 100 * high.mean(),
            "wait_p50_correct_us": wstat(ok, 50), "wait_p99_correct_us": wstat(ok, 99),
            "wait_p50_too_low_us": wstat(low, 50), "wait_p99_too_low_us": wstat(low, 99),
        })

    def key(r):
        return (r["workload"], r["scope"], ORDER.index(r["config"]) if r["config"] in ORDER else 99)
    rows.sort(key=key)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", newline="") as fh:
        wr = csv.DictWriter(fh, fieldnames=list(rows[0].keys()), lineterminator="\n")
        wr.writeheader()
        for r in rows:
            wr.writerow({k: (f"{v:.2f}" if isinstance(v, float) else v) for k, v in r.items()})

    cur = None
    for r in rows:
        if (r["workload"], r["scope"]) != cur:
            cur = (r["workload"], r["scope"])
            print(f"\n== {cur[0]} workload, {cur[1]} tasks")
            print(f"{'config':<16}{'bursts':>9}{'correct':>9}{'too low':>9}{'too high':>9}"
                  f"{'wait p99 ok':>13}{'wait p99 low':>14}")
        def us(v, w):
            return f"{'-':>{w}}  " if v != v else f"{v:>{w}.0f}us"
        print(f"{r['config']:<16}{r['bursts']:>9}{r['correct_pct']:>8.1f}%{r['too_low_pct']:>8.1f}%"
              f"{r['too_high_pct']:>8.1f}%{us(r['wait_p99_correct_us'], 11)}{us(r['wait_p99_too_low_us'], 12)}")
    print(f"\nwrote {args.out}")


if __name__ == "__main__":
    main()
