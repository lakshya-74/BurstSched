#!/usr/bin/env python3
"""
Train BurstSched's learned burst predictor (a small decision tree) from burst
logs written by `burstsched -o`, evaluate it against the EWMA baseline on
held-out logs, and export it for the loader (`burstsched -m model.txt`).

Usage:
  python predictor/train.py --train results/train_*.csv \
                            --test results/bursts_phase_mlfq_1.csv results/bursts_wakeup_mlfq_1.csv \
                            -o models/tree.txt
"""
import argparse
import csv
import glob
import math
import os
from collections import defaultdict

import numpy as np
from sklearn.tree import DecisionTreeRegressor

MSEC = 1_000_000
FEATURES = ["burst_t", "sleep_t", "ewma_t", "burst_t-1", "sleep_t-1"]


def level_for_burst(b):
    """Same thresholds as level_for_burst() in burstsched.bpf.c."""
    if b < 1 * MSEC:
        return 0
    if b < 4 * MSEC:
        return 1
    if b < 12 * MSEC:
        return 2
    return 3


def read_events(paths):
    """Return {(file, pid): [(burst, sleep), ...]} in time order."""
    tasks = defaultdict(list)
    for path in paths:
        with open(path) as f:
            rows = sorted(csv.DictReader(f), key=lambda r: int(r["ts_ns"]))
        for r in rows:
            tasks[(path, r["pid"])].append((int(r["burst_ns"]), int(r["sleep_ns"])))
    return tasks


def features(tasks):
    """
    Build (X, y, ewma_pred). MUST match learn_event() in src/burstsched.c:
    after burst t we know burst_t, sleep_t, EWMA (alpha 1/8, integer), burst_{t-1},
    sleep_{t-1}; the target is burst_{t+1}.
    """
    X, y, ew = [], [], []
    for evs in tasks.values():
        if len(evs) < 3:
            continue
        b1 = s1 = b2 = s2 = 0
        ewma = 0
        for i, (b, s) in enumerate(evs):
            b2, s2 = b1, s1
            b1, s1 = b, s
            if i == 0:
                ewma = b1
            elif b1 > ewma:
                ewma += (b1 - ewma) >> 3
            else:
                ewma -= (ewma - b1) >> 3
            if i + 1 < len(evs):
                X.append([b1, s1, ewma, b2, s2])
                y.append(evs[i + 1][0])
                ew.append(ewma)
    return np.array(X, dtype=float), np.array(y, dtype=float), np.array(ew, dtype=float)


def report(name, y, pred):
    err = np.abs(np.log2(pred + 1) - np.log2(y + 1))
    lvl_ok = np.mean([level_for_burst(a) == level_for_burst(b) for a, b in zip(y, pred)])
    within2x = np.mean((pred <= 2 * y) & (pred >= y / 2))
    print(f"  {name:<8} level-accuracy={lvl_ok*100:5.1f}%  within-2x={within2x*100:5.1f}%  "
          f"median|log2 err|={np.median(err):.2f}")


def expand(patterns):
    out = []
    for p in patterns:
        out.extend(sorted(glob.glob(p)) or [p])
    return [p for p in out if os.path.exists(p)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--train", nargs="+", required=True)
    ap.add_argument("--test", nargs="*", default=[])
    ap.add_argument("-o", "--out", default="models/tree.txt")
    ap.add_argument("--depth", type=int, default=6)
    ap.add_argument("--min-leaf", type=int, default=30)
    args = ap.parse_args()

    train_files = expand(args.train)
    test_files = [p for p in expand(args.test) if p not in train_files]
    print(f"train files: {len(train_files)}  test files: {len(test_files)}")

    X, y, ew = features(read_events(train_files))
    if len(y) < 100:
        raise SystemExit(f"only {len(y)} training samples; collect more data")
    print(f"training samples: {len(y)}")

    # regress on log2(burst): bursts span 1us..seconds
    model = DecisionTreeRegressor(max_depth=args.depth, min_samples_leaf=args.min_leaf,
                                  random_state=0)
    model.fit(X, np.log2(y + 1))

    # per-leaf prediction (ns) and expected error (median abs error, ns)
    leaves = model.apply(X)
    t = model.tree_
    val_ns = np.power(2.0, t.value[:, 0, 0]) - 1
    dev_ns = np.zeros(t.node_count)
    for leaf in np.unique(leaves):
        ys = y[leaves == leaf]
        dev_ns[leaf] = float(np.median(np.abs(ys - val_ns[leaf])))

    print("\ntraining-set accuracy")
    report("EWMA", y, ew)
    report("learned", y, np.power(2.0, model.predict(X)) - 1)

    for tf in test_files:
        Xt, yt, ewt = features(read_events([tf]))
        if len(yt) == 0:
            continue
        print(f"\nheld-out: {os.path.basename(tf)}  ({len(yt)} samples)")
        report("EWMA", yt, ewt)
        report("learned", yt, np.power(2.0, model.predict(Xt)) - 1)

    imp = sorted(zip(model.feature_importances_, FEATURES), reverse=True)
    print("\nfeature importance: " + ", ".join(f"{n}={v:.2f}" for v, n in imp))

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        f.write(f"{t.node_count}\n")
        for i in range(t.node_count):
            leaf = t.children_left[i] == -1
            feat = -1 if leaf else int(t.feature[i])
            thr = 0.0 if leaf else float(t.threshold[i])
            f.write(f"{feat} {thr:.1f} {int(t.children_left[i])} {int(t.children_right[i])} "
                    f"{val_ns[i]:.1f} {dev_ns[i]:.1f}\n")
    print(f"\nwrote {args.out}: {t.node_count} nodes, depth {model.get_depth()}")


if __name__ == "__main__":
    main()
