#!/usr/bin/env python3
"""
Report figures for BurstSched, from the curated data in data/final/.

  fig1_ablation.png        p99 latency of every configuration, one panel per workload
  fig2_wakeup_tail.png     wakeup-latency tail (CCDF) of the main schedulers
  fig3_phase_recovery.png  latency of each wakeup after a CPU-bound phase
  fig4_costs.png           context switches and worst-case wait per configuration
  fig5_predictor.png       held-out accuracy of EWMA vs the learned predictor
  fig6_misplacement.png    how often the phase task was queued too low, and what it cost

Usage: python analysis/plots.py [data_dir] [out_dir]     (defaults: data/final figures)
"""
import csv
import glob
import json
import os
import statistics as st
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter  # noqa: E402

DATA = sys.argv[1] if len(sys.argv) > 1 else "data/final"
OUT = sys.argv[2] if len(sys.argv) > 2 else "figures"
os.makedirs(OUT, exist_ok=True)

# ---- palette (dataviz reference palette, light mode; validated) ----
SURFACE = "#fcfcfb"
INK, INK2, MUTED = "#0b0b0b", "#52514e", "#898781"
GRID, AXIS = "#e1e0d9", "#c3c2b7"
REST = "#c3c2b7"            # de-emphasised bars ("gray the rest")
# entity colours, fixed across all figures (categorical slots 1-5, fixed order)
C_BURST = "#2a78d6"         # slot 1  BurstSched final (learned + per-CPU)
C_EEVDF = "#eb6834"         # slot 2  EEVDF
C_MLFQ = "#1baf7a"          # slot 3  reactive MLFQ
C_SIMPLE = "#eda100"        # slot 4  scx_simple
C_EWMA = "#e87ba4"          # slot 5  BurstSched with EWMA ("full")

plt.rcParams.update({
    "font.family": ["Segoe UI", "DejaVu Sans", "sans-serif"],
    "font.size": 9.5,
    "axes.facecolor": SURFACE, "figure.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": AXIS, "axes.labelcolor": INK2, "axes.titlecolor": INK,
    "axes.titlesize": 10.5, "axes.titleweight": "semibold", "axes.titlelocation": "left",
    "xtick.color": MUTED, "ytick.color": INK2, "xtick.labelcolor": INK2,
    "axes.grid": False, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.spines.top": False, "axes.spines.right": False,
    "legend.frameon": False, "legend.labelcolor": INK2,
})

CONFIGS = ["eevdf", "scx_simple", "mlfq", "ewma", "ewma_conf", "ewma_conf_slc",
           "full", "learned", "learned_percpu"]
LABEL = {
    "eevdf": "EEVDF (Linux default)", "scx_simple": "scx_simple (minimal sched_ext)",
    "mlfq": "Reactive MLFQ", "ewma": "+ EWMA prediction", "ewma_conf": "+ confidence gating",
    "ewma_conf_slc": "+ predicted slices", "full": "+ wakeup preemption",
    "learned": "+ learned predictor", "learned_percpu": "+ per-CPU queues",
}
FINAL = "learned_percpu"


def fmt_us(v, _pos=None):
    """Axis/label formatter for values in microseconds."""
    if v >= 1000:
        ms = v / 1000
        return f"{ms:.0f} ms" if ms >= 10 or ms == int(ms) else f"{ms:.1f} ms"
    return f"{v:.0f} µs" if v >= 10 or v == int(v) else f"{v:.1f} µs"


def log_axis(ax, which="x"):
    axis = ax.xaxis if which == "x" else ax.yaxis
    (ax.set_xscale if which == "x" else ax.set_yscale)("log")
    axis.set_major_locator(LogLocator(base=10))
    axis.set_major_formatter(FuncFormatter(fmt_us))
    axis.set_minor_formatter(NullFormatter())
    ax.grid(True, axis=which, which="major")
    ax.set_axisbelow(True)


def save(fig, name):
    for ext in ("png", "svg"):
        fig.savefig(os.path.join(OUT, f"{name}.{ext}"), dpi=200, bbox_inches="tight")
    plt.close(fig)
    print("wrote", os.path.join(OUT, name + ".png"))


def summary(workload):
    path = os.path.join(DATA, f"summary_{workload}_full.csv")
    if not os.path.exists(path):
        return None
    rows = {}
    for r in csv.DictReader(open(path)):
        rows.setdefault(r["config"], []).append(r)

    def med(cfg, col):
        vals = []
        for r in rows.get(cfg, []):
            try:
                vals.append(float(r.get(col, "NA")))
            except ValueError:
                pass
        return st.median(vals) if vals else None
    return med


def hbar_panel(ax, med, col, title, xlabel):
    cfgs = [c for c in CONFIGS if med(c, col) is not None]
    vals = [med(c, col) for c in cfgs]
    y = np.arange(len(cfgs))[::-1]
    colors = [C_BURST if c == FINAL else REST for c in cfgs]
    ax.barh(y, vals, height=0.62, color=colors, edgecolor=SURFACE, linewidth=1.5)
    log_axis(ax, "x")
    ax.set_xlim(min(vals) / 2.5, max(vals) * 6)
    for yi, v, c in zip(y, vals, cfgs):
        ax.text(v * 1.12, yi, fmt_us(v), va="center", ha="left", fontsize=8.5,
                color=INK if c == FINAL else INK2,
                fontweight="semibold" if c == FINAL else "normal")
    ax.set_yticks(y)
    ax.set_yticklabels([LABEL[c] for c in cfgs])
    ax.tick_params(axis="y", length=0)
    ax.set_title(title)
    ax.set_xlabel(xlabel)
    return cfgs


# ---------------- fig 1: ablation ----------------
def fig1():
    panels = [("wakeup", "p99_us", "Steady interactive task", "p99 wakeup latency (log scale)"),
              ("phase", "rec_p99_us", "Phase-change recovery", "p99 latency after a CPU phase (log scale)"),
              ("rr", "p99_us", "Request/response service", "p99 request latency (log scale)")]
    panels = [p for p in panels if summary(p[0])]
    fig, axes = plt.subplots(1, len(panels), figsize=(4.2 * len(panels) + 1.6, 4.0),
                             sharey=True, gridspec_kw={"wspace": 0.12})
    axes = np.atleast_1d(axes)
    for ax, (wl, col, title, xl) in zip(axes, panels):
        hbar_panel(ax, summary(wl), col, title, xl)
    fig.suptitle("Ablation: each feature added in turn (72 CPU hogs on 36 CPUs, median of 3 runs)",
                 x=0.01, ha="left", fontsize=11.5, fontweight="semibold", color=INK, y=1.02)
    save(fig, "fig1_ablation")


# ---------------- per-wakeup latency files ----------------
def lat_files(workload, cfg):
    return sorted(glob.glob(os.path.join(DATA, f"lat_{workload}_full_{cfg}_[0-9]*.csv")))


SERIES = [("eevdf", "EEVDF", C_EEVDF, "o"), ("scx_simple", "scx_simple", C_SIMPLE, "D"),
          ("mlfq", "Reactive MLFQ", C_MLFQ, "s"), ("full", "BurstSched, EWMA", C_EWMA, "^"),
          ("learned_percpu", "BurstSched, learned + per-CPU", C_BURST, "o")]


# ---------------- fig 2: wakeup tail ----------------
def fig2():
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    ends = []
    for cfg, name, color, _m in SERIES:
        lat = []
        for f in lat_files("wakeup", cfg):
            lat += [int(r["latency_ns"]) / 1000 for r in csv.DictReader(open(f))]
        if not lat:
            continue
        x = np.sort(np.maximum(np.array(lat), 0.5))
        ccdf = 1.0 - np.arange(len(x)) / len(x)
        ax.plot(x, ccdf * 100, color=color, linewidth=2, label=name, solid_capstyle="round")
        ends.append((x[-1], ccdf[-1] * 100, name, color))
    log_axis(ax, "x")
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g}%"))
    ax.yaxis.set_minor_formatter(NullFormatter())
    ax.grid(True, axis="y", which="major")
    ax.set_ylim(100 / 12000, 130)
    ax.set_xlabel("wakeup latency (log scale)")
    ax.set_ylabel("share of wakeups slower than this")
    ax.set_title("Wakeup-latency tail, steady interactive task (9,000 wakeups per scheduler)")
    # 5 series: legend only (the lines all end at the same 1-in-9000 level, so
    # end labels would collide); the values are in the report's tables
    ax.legend(loc="lower left", fontsize=8.5)
    save(fig, "fig2_wakeup_tail")


def label_ends(ax, items, min_gap=0.13):
    """Direct labels at line ends, nudged apart (in log10 units) so none overlap."""
    items = sorted(items, key=lambda t: np.log10(t[1]))
    placed = []
    for x, y, text in items:
        ly = np.log10(y)
        if placed and ly - placed[-1] < min_gap:
            ly = placed[-1] + min_gap
        placed.append(ly)
        ax.annotate(text, (x, 10 ** ly), xytext=(7, 0), textcoords="offset points",
                    fontsize=8, color=INK2, va="center")


# ---------------- fig 3: phase recovery ----------------
def fig3(nidx=20):
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    ends = []
    for cfg, name, color, marker in SERIES:
        if cfg == "scx_simple":
            continue
        by_idx = [[] for _ in range(nidx)]
        for f in lat_files("phase", cfg):
            for r in csv.DictReader(open(f)):
                cy, i = int(r["cycle"]), int(r["idx"])
                if cy >= 1 and i < nidx:
                    by_idx[i].append(int(r["latency_ns"]) / 1000)
        if not by_idx[0]:
            continue
        med = [max(np.median(v), 0.5) for v in by_idx]
        xs = np.arange(1, nidx + 1)
        ax.plot(xs, med, color=color, linewidth=2, marker=marker, markersize=6,
                markeredgecolor=SURFACE, markeredgewidth=1.5, label=name)
        ends.append((xs[-1], med[-1], name))
    label_ends(ax, ends)
    log_axis(ax, "y")
    ax.set_xlim(0.5, nidx + 6.5)
    ax.set_xticks([1, 5, 10, 15, 20])
    ax.set_xlabel("wakeup number after the CPU-bound phase ends")
    ax.set_ylabel("median latency (log scale)")
    ax.set_title("Recovery after a 2 s CPU-bound phase (cycles 2-10, 3 runs)")
    ax.legend(loc="upper right", fontsize=8.5, bbox_to_anchor=(1.0, 0.92), numpoints=1)
    save(fig, "fig3_phase_recovery")


# ---------------- fig 4: costs ----------------
def fig4():
    med = summary("wakeup")
    wait_col = "wm_max_hog_ms" if med("eevdf", "wm_max_hog_ms") is not None else "max_wait_ms"
    fig, (a, b) = plt.subplots(1, 2, figsize=(11.0, 4.0), sharey=True,
                               gridspec_kw={"wspace": 0.12})
    cfgs = [c for c in CONFIGS if med(c, "ctx_switches") is not None]
    y = np.arange(len(cfgs))[::-1]
    for ax, col, title, unit in (
            (a, "ctx_switches", "Context switches during the run", "thousands"),
            (b, wait_col, "Worst-case wait of a CPU hog", "ms")):
        vals = [med(c, col) for c in cfgs]
        colors = [C_BURST if c == FINAL else REST for c in cfgs]
        shown = [v if v is not None else 0 for v in vals]
        if col == "ctx_switches":
            shown = [v / 1000 for v in shown]
        ax.barh(y, shown, height=0.62, color=colors, edgecolor=SURFACE, linewidth=1.5)
        top = max(shown) or 1
        for yi, v, s, c in zip(y, vals, shown, cfgs):
            txt = "not measured" if v is None else (f"{s:.0f}k" if col == "ctx_switches" else f"{s:.0f} ms")
            ax.text(s + top * 0.02, yi, txt, va="center", fontsize=8.5,
                    color=INK if c == FINAL else (MUTED if v is None else INK2),
                    fontweight="semibold" if c == FINAL else "normal")
        ax.set_xlim(0, top * 1.25)
        ax.grid(True, axis="x")
        ax.set_axisbelow(True)
        ax.set_title(title)
        ax.set_xlabel(unit)
        ax.tick_params(axis="y", length=0)
    a.set_yticks(y)
    a.set_yticklabels([LABEL[c] for c in cfgs])
    note = "" if wait_col.startswith("wm_") else "  (wait measured inside BurstSched only)"
    fig.suptitle("Costs, steady interactive workload" + note, x=0.01, ha="left",
                 fontsize=11.5, fontweight="semibold", color=INK, y=1.02)
    save(fig, "fig4_costs")


# ---------------- fig 5: predictor accuracy ----------------
def fig5():
    path = os.path.join(DATA, "predictor_eval.json")
    if not os.path.exists(path):
        return
    m = json.load(open(path))
    names = {"bursts_phase_mlfq_1.csv": "phase workload\n(partial mode)",
             "bursts_wakeup_mlfq_1.csv": "wakeup workload\n(partial mode)",
             "bursts_phase_full_mlfq_1.csv": "phase workload\n(full mode, all tasks)",
             "bursts_wakeup_full_mlfq_1.csv": "wakeup workload\n(full mode, all tasks)"}
    sets = [k for k in names if k in m["held_out"]]
    fig, axes = plt.subplots(1, 2, figsize=(11.0, 4.0), sharey=True, gridspec_kw={"wspace": 0.1})
    for ax, scope, title in ((axes[0], "all", "All bursts"),
                             (axes[1], "phase_change", "Bursts right after a behaviour change")):
        y = np.arange(len(sets))[::-1]
        for off, pred, color, lab in ((0.17, "EWMA", REST, "EWMA (textbook)"),
                                      (-0.17, "learned", C_BURST, "Learned predictor")):
            vals = [m["held_out"][s].get(scope, {}).get(pred, {}).get("level_acc", 0) for s in sets]
            ax.barh(y + off, vals, height=0.32, color=color, edgecolor=SURFACE, linewidth=1.5, label=lab)
            for yi, v in zip(y + off, vals):
                ax.text(v + 1.2, yi, f"{v:.0f}%", va="center", fontsize=8.5,
                        color=INK if pred == "learned" else INK2)
        ax.set_xlim(0, 112)
        ax.set_xticks([0, 25, 50, 75, 100])
        ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:.0f}%"))
        ax.grid(True, axis="x")
        ax.set_axisbelow(True)
        ax.set_title(title)
        ax.set_xlabel("next burst placed in the correct queue level")
        ax.tick_params(axis="y", length=0)
    axes[0].set_yticks(np.arange(len(sets))[::-1])
    axes[0].set_yticklabels([names[s] for s in sets])
    axes[1].legend(loc="upper right", fontsize=8.5, bbox_to_anchor=(1.0, 1.02))
    fig.suptitle("Predictor accuracy on workloads never seen in training", x=0.01, ha="left",
                 fontsize=11.5, fontweight="semibold", color=INK, y=1.02)
    save(fig, "fig5_predictor")


# ---------------- fig 6: misplacement ----------------
def fig6():
    path = os.path.join(DATA, "mispredict.csv")
    if not os.path.exists(path):
        return
    rows = [r for r in csv.DictReader(open(path))
            if r["workload"] == "phase" and r["scope"] == "workload"]
    order = [c for c in CONFIGS if any(r["config"] == c for r in rows)]
    rows = {r["config"]: r for r in rows}
    fig, (a, b) = plt.subplots(1, 2, figsize=(11.0, 3.6), sharey=True, gridspec_kw={"wspace": 0.12})
    y = np.arange(len(order))[::-1]
    low = [float(rows[c]["too_low_pct"]) for c in order]
    cost = [float(rows[c]["wait_p99_too_low_us"]) for c in order]
    colors = [C_BURST if c in ("learned", FINAL) else REST for c in order]
    a.barh(y, low, height=0.62, color=colors, edgecolor=SURFACE, linewidth=1.5)
    for yi, v in zip(y, low):
        a.text(v + 0.03, yi, f"{v:.1f}%", va="center", fontsize=8.5, color=INK2)
    a.set_xlim(0, max(low) * 1.3)
    a.xaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g}%"))
    a.set_title("Phase-task bursts queued too low")
    a.set_xlabel("share of the task's bursts")
    shown = [c / 1000 if c == c else 0 for c in cost]
    b.barh(y, shown, height=0.62, color=colors, edgecolor=SURFACE, linewidth=1.5)
    for yi, v, raw in zip(y, shown, cost):
        b.text(v + 0.6, yi, "none" if raw != raw else f"{v:.0f} ms", va="center", fontsize=8.5,
               color=MUTED if raw != raw else INK2)
    b.set_xlim(0, max(shown) * 1.25)
    b.set_title("Cost: p99 queue wait of those bursts")
    b.set_xlabel("ms")
    for ax in (a, b):
        ax.grid(True, axis="x")
        ax.set_axisbelow(True)
        ax.tick_params(axis="y", length=0)
    a.set_yticks(y)
    a.set_yticklabels([LABEL[c] for c in order])
    fig.suptitle("Misplacement after phase changes (full mode, 3 runs)", x=0.01, ha="left",
                 fontsize=11.5, fontweight="semibold", color=INK, y=1.03)
    save(fig, "fig6_misplacement")


if __name__ == "__main__":
    fig1()
    fig2()
    fig3()
    fig4()
    fig5()
    fig6()
