# Final ablation (full mode)

Lab PC B: 36 CPUs, Ubuntu 24.04.4, kernel 6.17.0-1033-oem, 2026-11-22.
Full mode: the BPF scheduler handles **every** task on the machine. 72 `stress-ng --cpu`
hogs (2 per CPU) run during every measurement. 3 repetitions per configuration; the
tables show medians. Raw data: `data/final/` (`summary_*_full.csv` = one row per run,
`lat_*_full_<config>_<rep>.csv` = one row per wakeup). Command: `scripts/final_ablation.sh`.

## Configurations

| config | what it adds | loader flags |
|---|---|---|
| eevdf | default Linux scheduler (no BPF scheduler) | n/a |
| scx_simple | minimal sched_ext example scheduler (global vtime queue) | n/a |
| mlfq | reactive 4-level MLFQ + aging, no prediction | (none) |
| ewma | + EWMA burst prediction | `-e` |
| ewma_conf | + confidence gating | `-e -c` |
| ewma_conf_slc | + predicted time slices | `-e -c -s` |
| full | + wakeup preemption by predicted burst | `-e -c -s -w` |
| learned | EWMA replaced by the learned predictor | `-m models/tree.txt -c -s -w` |
| learned_percpu | + per-CPU queues, burst-aware placement, work stealing | `-m models/tree.txt -c -s -w -P` |

## Wakeup workload

`wakeup_lat`: sleeps 10 ms, runs 200 us, repeated 3000 times (a steady interactive task).

| config | p50 us | p99 us | max us | hog ops/s | ctx switches | Jain | max wait ms | preemptions |
|---|---|---|---|---|---|---|---|---|
| eevdf | 5.6 | 3359.3 | 6360 | 63401 | 333503 | 0.9883 | NA | NA |
| scx_simple | 1798.7 | 10808.8 | 11808 | 63507 | 72280 | 1.0000 | NA | NA |
| mlfq | 809.4 | 8799.8 | 10805 | 63536 | 84441 | 1.0000 | 26.0 | 0 |
| ewma | 805.8 | 9808.2 | 13007 | 63499 | 86069 | 1.0000 | 22.0 | 0 |
| ewma_conf | 297.6 | 5080.2 | 6085 | 63579 | 85777 | 1.0000 | 21.6 | 0 |
| ewma_conf_slc | 1268.4 | 8266.6 | 10805 | 63448 | 43796 | 1.0000 | 52.0 | 0 |
| full | 8.8 | 142.4 | 3970 | 63656 | 56374 | 0.9999 | 41.0 | 10429 |
| learned | 10.2 | 38.1 | 286 | 63604 | 53433 | 0.9999 | 41.1 | 8622 |
| learned_percpu | 7.5 | **11.1** | 2092 | 63602 | 53997 | 0.9999 | 82.0 | 9256 |

## Phase-change workload

`phase_change`: 10 cycles of [2 s CPU-bound, 2 s interactive (10 ms sleep / 200 us run)].
`rec p99` = p99 of the first 10 wakeups after each CPU phase in cycles 2-10 (how fast the
scheduler notices the task became interactive again). `cold` = the very first wakeup.

| config | p50 us | p99 us | max us | rec p99 us | cold us | ctx switches | Jain | max wait ms |
|---|---|---|---|---|---|---|---|---|
| eevdf | 5.8 | 3003.8 | 4662 | 4000.5 | 442.3 | 434679 | 0.9948 | NA |
| scx_simple | 3793.3 | 10814.3 | 12818 | 12818.0 | 5146.8 | 80412 | 1.0000 | NA |
| mlfq | 800.6 | 6800.4 | 22231 | 21808.7 | 22230.8 | 109424 | 1.0000 | 29.0 |
| ewma | 800.1 | 10800.1 | 22003 | 22002.8 | 18997.7 | 108842 | 1.0000 | 28.7 |
| ewma_conf | 800.8 | 3805.6 | 20295 | 18803.4 | 20295.4 | 110618 | 1.0000 | 24.0 |
| ewma_conf_slc | 1793.9 | 10804.3 | 47024 | 45001.3 | 47023.9 | 54728 | 1.0000 | 55.0 |
| full | 15.8 | 6787.3 | 39013 | 39012.9 | 35558.3 | 68773 | 0.9999 | 47.0 |
| learned | 10.0 | 31.5 | 86 | 29.2 | 8.1 | 65572 | 0.9999 | 50.0 |
| learned_percpu | 7.7 | **14.9** | **28** | **17.8** | 12.1 | 66535 | 0.9998 | 82.2 |

"max wait" is measured inside BurstSched, so it is NA for eevdf and scx_simple.

## What each feature contributed

| step | effect |
|---|---|
| reactive MLFQ | Already classifies a steady interactive task correctly; p99 is bounded by the hogs' slice ends (~9 ms). |
| + EWMA | No measurable gain on its own (within noise). |
| + confidence gating | Helps: wakeup p99 9.8 -> 5.1 ms. Rejects unstable predictions. |
| + predicted slices | Halves context switches (86k -> 44k) but, without preemption, raises latency and worst-case wait (22 -> 52 ms). |
| + wakeup preemption | The big win for steady tasks: p99 8.3 ms -> 142 us. Phase-change recovery still ~39 ms. |
| + learned predictor | Fixes phase changes: recovery p99 39 ms -> 29 us; steady-task p99 142 -> 38 us. |
| + per-CPU queues | Best tail latency (11 us wakeup p99, 28 us phase max) at the cost of a higher worst-case wait (82 ms). |

Hog throughput is the same for every configuration (medians: wakeup test 63.4k-63.7k
bogo ops/s, phase test 62.9k-63.2k; within 0.5% in each test).
Every BurstSched configuration is at least as fair as EEVDF (Jain >= 0.9998 vs 0.988-0.995).

## How often does prediction miss, and what does it cost?

From the burst logs of the same runs (`analysis/mispredict.py`, `data/final/mispredict.csv`).
For each finished burst of the measured task, the queue level it was placed in is compared
with the level its real burst length called for.

| config | phase task: bursts queued too low | p99 queue wait of those bursts | wakeup task: placed correctly |
|---|---|---|---|
| mlfq | 1.5% | 23 ms | 100% |
| ewma | 1.5% | 22 ms | 100% |
| ewma_conf | 1.5% | 20 ms | 100% |
| ewma_conf_slc | 1.6% | 49 ms | 100% |
| full | 1.5% | 39 ms | 100% |
| learned | **0.0%** | none | 100% |
| learned_percpu | **0.0%** | none | 100% |

- The 1.5% are the ~3 bursts after every CPU phase that reactive placement needs to climb
  from the lowest level back to the top; each costs a full queue wait behind CPU hogs.
  The learned predictor removes them.
- The steady task is placed correctly by **every** configuration, yet its p99 latency varies
  1000-fold (8.8 ms -> 11 us). Correct placement is not enough: without wakeup preemption a
  correctly placed task still waits for a CPU hog's slice to end.

## Predictor accuracy on held-out data

`predictor/train.py --metrics data/final/predictor_eval.json`: share of next bursts placed in
the correct queue level, on burst logs the predictor was never trained on.

| held-out log | EWMA, all | learned, all | EWMA, after a behaviour change | learned, after a behaviour change |
|---|---|---|---|---|
| phase workload, partial mode | 74.8% | 97.2% | 0.0% | 68.9% |
| wakeup workload, partial mode | 95.9% | 98.4% | 0.0% | 62.2% |
| phase workload, full mode (all tasks) | 93.1% | 99.1% | 54.4% | 84.6% |
| wakeup workload, full mode (all tasks) | 97.9% | 99.1% | 53.9% | 82.5% |

## Figures

`python analysis/plots.py` writes `figures/fig1_ablation` ... `fig6_misplacement` (PNG + SVG).

## Known limitations

- **Cold start.** In partial mode (task joins the scheduler just before its first CPU phase)
  the first CPU-to-interactive switch costs one long queue wait (~42 ms), because there is no
  history to learn from (docs/run3_learned.md). In full mode the process's own start-up
  bursts provide that history, and the first switch is fast (8-12 us).
- **Worst-case wait.** Long slices (40 ms) and per-CPU queues raise the longest time a CPU-bound
  task waits (41-82 ms vs 21-29 ms for plain MLFQ). That is the price of fewer context switches.
- **Workloads are synthetic.** Two workload types on one machine; no real interactive
  application was measured.
