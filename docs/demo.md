# BurstSched demo guide

Run the commands below from the repository root. Choose a saved-results demo on any
machine with Python 3, or a live scheduler demo on a compatible Linux lab machine.

## Show the saved results

The repository includes measured data in `data/final/` and ready-to-show graphs in
[`figures/`](../figures/). You can print summary tables using only Python's standard
library, without root access or a Linux scheduler build.

On Windows (PowerShell):

```powershell
python analysis/summarize.py data/final/summary_wakeup_full.csv
python analysis/summarize.py data/final/summary_phase_full.csv
python analysis/summarize.py data/final/summary_rr_full.csv
```

On Linux or macOS:

```bash
python3 analysis/summarize.py data/final/summary_wakeup_full.csv
python3 analysis/summarize.py data/final/summary_phase_full.csv
python3 analysis/summarize.py data/final/summary_rr_full.csv
```

Each row shows the median and min-max spread over repetitions. These are recorded
experiments on a 36-CPU machine with 72 CPU hogs, in full mode; printing them does
not perform a new benchmark. See the [experiment details](final_ablation.md).

For a presentation, start with the [ablation graph](../figures/fig1_ablation.png), then
show [phase recovery](../figures/fig3_phase_recovery.png) and the
[scheduling costs](../figures/fig4_costs.png).

## Install Python dependencies

Plotting and predictor training use NumPy, Matplotlib, and scikit-learn, listed in
[`requirements.txt`](../requirements.txt). Install them in a virtual environment;
these packages are separate from the Linux build tools installed by `setup_lab.sh`.
On Ubuntu, install `python3-venv` first if `python3 -m venv` reports it is missing.

Windows (PowerShell):

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -m pip check
```

Linux or macOS:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python -m pip check
```

The commands use the virtual environment's Python directly, so shell activation is
optional. `.venv/` is ignored by Git. Installing packages requires internet access;
the saved-data analysis below runs locally after installation.

## Regenerate figures

Use an output directory under `results/` to keep generated files separate from the
published figures. No graphical desktop is required: the plotting script uses
Matplotlib's noninteractive backend.

Windows (PowerShell):

```powershell
.\.venv\Scripts\python.exe analysis/plots.py data/final results/demo-figures
.\.venv\Scripts\python.exe analysis/plots.py data/final results/demo-paper --paper
```

Linux or macOS:

```bash
.venv/bin/python analysis/plots.py data/final results/demo-figures
.venv/bin/python analysis/plots.py data/final results/demo-paper --paper
```

The first command generates six PNG/SVG figure pairs; the second generates six
PDF/PNG pairs sized for the paper. Fonts and package versions may change their
appearance. Missing optional fonts can print `findfont` warnings; Matplotlib uses
available fallback fonts. These commands visualize the included data; they do not
rerun experiments.

The dependencies also support `predictor/train.py`. Retraining requires raw burst
logs collected on Linux; the original `train_*.csv` logs are not included in the
curated data. See [`scripts/collect_training.sh`](../scripts/collect_training.sh)
and the usage example in [`predictor/train.py`](../predictor/train.py). The checked-in
`models/tree.txt` can be used for the live demo without retraining.

## Run a short live comparison (Linux)

Prepare a lab machine before the presentation, following the
[build instructions](../README.md#build-and-run-linux). The scheduler requires Linux
6.12 or newer, `CONFIG_SCHED_CLASS_EXT=y`, BTF, and root access. Windows and macOS can
display saved results, but cannot run this scheduler natively.

After `scripts/setup_lab.sh` completes successfully, run:

```bash
sudo env MODE=partial WORKLOAD=wakeup CONFIGS="eevdf mlfq learned_percpu" REPS=1 ITERS=1000 \
  ./scripts/quick_compare.sh
```

This compares the default Linux scheduler, reactive MLFQ, and BurstSched with the
learned predictor and per-CPU queues. It runs one repetition per configuration,
generates CPU load (two CPU hogs per logical CPU by default), prints a summary, and
writes `results/summary_wakeup.csv`. Allow a few minutes; duration depends on load
and hardware. Run it on a lab machine available for the test.

Partial mode applies BurstSched only to tasks launched through `build/setext`;
the comparison script does this for the CPU hogs and measured workload. The script
starts and stops the schedulers itself, so do not start a separate BurstSched instance.
Each invocation overwrites the same summary and per-run output paths; copy results
elsewhere before repeating a run you want to keep.

For an additional phase-change demonstration:

```bash
sudo env MODE=partial WORKLOAD=phase CONFIGS="eevdf mlfq learned_percpu" REPS=1 \
  ./scripts/quick_compare.sh
```

This writes `results/summary_phase.csv`. `ITERS` controls only the wakeup workload;
the phase workload uses ten CPU-bound/interactive cycles instead.

## Explain the output

| Field | Meaning | What to look for |
|---|---|---|
| `p99_us` | Wakeup or request latency at the 99th percentile, in microseconds | Lower tail latency |
| `rec_p99_us` | Latency of the first wakeups after CPU-bound phases, excluding the cold cycle | Faster recovery when a task becomes interactive |
| `hog_ops_per_s` | CPU-hog throughput reported by stress-ng | Whether throughput stays similar |
| `ctx_switches` | System-wide context-switch count during the measurement | Scheduling overhead; other activity also contributes |
| `jain` | Fairness of CPU time between the CPU hogs | Values closer to 1 mean more equal CPU time |
| `wm_max_hog_ms` | Longest runnable-but-not-running wait of a CPU hog, measured by waitmon | The cost paid by CPU-heavy tasks |

The configuration named `full` means EWMA, confidence gating, predicted slices, and
wakeup preemption. It is distinct from `MODE=full`, which makes the selected BPF
scheduler handle all eligible tasks on the machine. `learned_percpu` selects all
BurstSched features, including the learned predictor and per-CPU queues.

Use the recorded data to explain the measured tradeoff: much lower interactive tail
latency with similar CPU throughput, at the cost of longer worst-case waits for CPU
hogs. A short partial-mode run demonstrates the workflow; it does not reproduce the
three-repetition full-mode study. Live values depend on hardware, kernel, and load.
