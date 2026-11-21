#!/usr/bin/env python3
"""
Train BurstSched's learned burst predictor from burst logs (`burstsched -o`),
evaluate it against EWMA on held-out logs, and export it for the loader
(`burstsched -m models/tree.txt`).

Design (scale-free, so it generalises to burst sizes never seen in training):
after each finished burst t of a task, a small decision-tree CLASSIFIER picks
which of the task's own history-based estimates to trust for burst t+1:

    candidate 0 = burst_t          (last burst)
    candidate 1 = EWMA_t           (textbook exponential average)
    candidate 2 = burst_{t-1}      (burst before last; recovers after a phase change)

using only log-ratio features (no absolute sizes). Each leaf also stores the
median relative error of its choice, used as the confidence for gating.

Usage:
  python predictor/train.py --train "results/train_*.csv" \
      --test results/bursts_phase_mlfq_1.csv results/bursts_wakeup_mlfq_1.csv \
      -o models/tree.txt
"""
import argparse
import csv
import glob
import os
from collections import defaultdict

import numpy as np
from sklearn.tree import DecisionTreeClassifier

MSEC = 1_000_000
FEATURES = ["b1/ewma", "b1/b2", "s1/b1", "s2/b2", "s1/s2"]
CANDIDATES = ["last", "ewma", "prev"]


def L(x):
    """Approximate log2(x+1). MUST match flog2() in src/burstsched.c exactly."""
    v = int(x) + 1
    n = v.bit_length() - 1
    return n + (v - (1 << n)) / (1 << n)


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
            rd = csv.reader(f)
            hdr = next(rd)
            it, ip, ib, isl = (hdr.index(c) for c in ("ts_ns", "pid", "burst_ns", "sleep_ns"))
            rows = [(int(r[it]), r[ip], int(r[ib]), int(r[isl])) for r in rd]
        rows.sort()
        for _, pid, b, s in rows:
            tasks[(path, pid)].append((b, s))
    return tasks


def features(tasks, cap=0):
    """
    Returns X (features), C (candidate values, n x 3), y (true next burst).
    MUST match learn_event() in src/burstsched.c: history update, integer EWMA
    (alpha 1/8), and "only predict once two bursts are known" (i >= 1).
    """
    X, C, y = [], [], []
    for evs in tasks.values():
        if len(evs) < 3:
            continue
        step = max(1, (len(evs) - 1) // cap) if cap else 1
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
            if i >= 1 and i + 1 < len(evs) and i % step == 0:
                X.append([L(b1) - L(ewma), L(b1) - L(b2), L(s1) - L(b1),
                          L(s2) - L(b2), L(s1) - L(s2)])
                C.append([b1, ewma, b2])
                y.append(evs[i + 1][0])
    return np.array(X, float), np.array(C, float), np.array(y, float)


def best_candidate(C, y):
    err = np.abs(np.log2(C + 1) - np.log2(y + 1)[:, None])
    return np.argmin(err, axis=1)


def report(name, y, pred, mask=None):
    if mask is not None:
        y, pred = y[mask], pred[mask]
    if len(y) == 0:
        return
    lvl = np.mean([level_for_burst(a) == level_for_burst(b) for a, b in zip(y, pred)])
    w2 = np.mean((pred <= 2 * y) & (pred >= y / 2))
    err = np.median(np.abs(np.log2(pred + 1) - np.log2(y + 1)))
    print(f"    {name:<8} level-accuracy={lvl*100:5.1f}%  within-2x={w2*100:5.1f}%  "
          f"median|log2 err|={err:.2f}  (n={len(y)})")


def evaluate(title, X, C, y, model):
    pick = model.predict(X)
    learned = C[np.arange(len(y)), pick]
    # "shift" = the last burst differs from the one before by >= 8x (phase change)
    shift = np.abs(X[:, 1]) >= 3
    print(f"\n{title}")
    print("  all samples:")
    report("EWMA", y, C[:, 1])
    report("last", y, C[:, 0])
    report("learned", y, learned)
    if shift.any():
        print("  phase-change samples (last burst 8x+ different from previous):")
        report("EWMA", y, C[:, 1], shift)
        report("last", y, C[:, 0], shift)
        report("learned", y, learned, shift)


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
    ap.add_argument("--depth", type=int, default=5)
    ap.add_argument("--min-leaf", type=int, default=30)
    ap.add_argument("--cap", type=int, default=3000,
                    help="max training samples per task (0 = no cap)")
    args = ap.parse_args()

    train_files = expand(args.train)
    test_files = [p for p in expand(args.test) if p not in train_files]
    print(f"train files: {len(train_files)}  test files: {len(test_files)}")

    X, C, y = features(read_events(train_files), cap=args.cap)
    if len(y) < 100:
        raise SystemExit(f"only {len(y)} training samples; collect more data")
    labels = best_candidate(C, y)
    print(f"training samples: {len(y)}  best-candidate mix: " +
          ", ".join(f"{CANDIDATES[k]}={np.mean(labels == k)*100:.0f}%" for k in range(3)))

    model = DecisionTreeClassifier(max_depth=args.depth, min_samples_leaf=args.min_leaf,
                                   random_state=0)
    model.fit(X, labels)

    # per-leaf chosen candidate and its median relative error (confidence)
    t = model.tree_
    leaves = model.apply(X)
    cls = np.argmax(t.value[:, 0, :], axis=1)
    relerr = np.zeros(t.node_count)
    for leaf in np.unique(leaves):
        m = leaves == leaf
        c = C[m, cls[leaf]]
        relerr[leaf] = float(np.median(np.abs(y[m] - c) / np.maximum(c, 1)))

    evaluate("training set", X, C, y, model)
    for tf in test_files:
        Xt, Ct, yt = features(read_events([tf]))
        if len(yt):
            evaluate(f"HELD-OUT: {os.path.basename(tf)}", Xt, Ct, yt, model)

    imp = sorted(zip(model.feature_importances_, FEATURES), reverse=True)
    print("\nfeature importance: " + ", ".join(f"{n}={v:.2f}" for v, n in imp))

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", newline="\n") as f:
        f.write(f"{t.node_count}\n")
        for i in range(t.node_count):
            leaf = t.children_left[i] == -1
            feat = -1 if leaf else int(t.feature[i])
            thr = 0.0 if leaf else float(t.threshold[i])
            f.write(f"{feat} {thr:.9f} {int(t.children_left[i])} {int(t.children_right[i])} "
                    f"{int(cls[i])} {relerr[i]:.6f}\n")
    print(f"wrote {args.out}: {t.node_count} nodes, depth {model.get_depth()}")


if __name__ == "__main__":
    main()
