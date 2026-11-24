# Final ablation (full mode)

Lab PC B: 36 CPUs, Ubuntu 24.04, kernel 6.17.0-1033-oem, 2026-11-24.
Full mode: the BPF scheduler handles **every** task on the machine. 72 `stress-ng --cpu`
hogs (2 per CPU) run during every measurement. 3 repetitions per configuration; tables show
medians. Raw data: `data/final/` (`summary_<workload>_full.csv` = one row per run,
`lat_<workload>_full_<config>_<rep>.csv` = one row per wakeup/request,
`wait_*.txt` = waitmon output). Command: `scripts/final_ablation.sh` (~55 min).

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

**Worst-case wait** is measured by `waitmon` (`src/waitmon.bpf.c`), a tracer on the core
scheduler tracepoints, so it is measured the same way for every scheduler: the longest time
any CPU hog (or the measured task) was runnable but not running.

## Workload 1: steady interactive task

`wakeup_lat`: sleeps 10 ms, runs 200 us, repeated 3000 times.

| config | p50 us | p99 us | max us | hog ops/s | ctx switches | Jain | worst hog wait ms | worst task wait ms | preemptions |
|---|---|---|---|---|---|---|---|---|---|
| eevdf | 6.8 | 4046.1 | 6348 | 63457 | 338931 | 0.9935 | 20.0 | 6.3 | NA |
| scx_simple | 2785.6 | 10809.6 | 11799 | 63466 | 73408 | 1.0000 | 62.0 | 11.8 | NA |
| mlfq | 805.3 | 10796.8 | 10811 | 63514 | 86519 | 1.0000 | 23.0 | 10.8 | 0 |
| ewma | 822.4 | 10801.1 | 11790 | 63523 | 86362 | 1.0000 | 25.6 | 11.8 | 0 |
| ewma_conf | 890.4 | 6054.6 | 7888 | 63595 | 85774 | 1.0000 | 22.0 | 7.9 | 0 |
| ewma_conf_slc | 1811.8 | 10810.6 | 12783 | 63519 | 48785 | 1.0000 | 56.0 | 12.8 | 0 |
| full | 10.3 | 180.4 | 5312 | 63662 | 56706 | 0.9999 | 41.2 | 5.3 | 10852 |
| learned | 11.1 | 33.9 | 1175 | 63663 | 55000 | 0.9999 | 41.0 | 1.2 | 10086 |
| learned_percpu | 8.6 | **12.3** | 187 | 63624 | 55671 | 0.9999 | 82.0 | 1.9 | 10618 |

## Workload 2: phase change

`phase_change`: 10 cycles of [2 s CPU-bound, 2 s interactive (10 ms sleep / 200 us run)].
`rec p99` = p99 of the first 10 wakeups after each CPU phase in cycles 2-10 (how fast the
scheduler notices the task became interactive again). `cold` = the very first wakeup.

| config | p50 us | p99 us | max us | rec p99 us | cold us | ctx switches | Jain | worst hog wait ms |
|---|---|---|---|---|---|---|---|---|
| eevdf | 6.7 | 3003.3 | 5001 | 3997.1 | 10.0 | 442665 | 0.9935 | 18.0 |
| scx_simple | 7728.9 | 10819.5 | 10897 | 10802.1 | 8963.6 | 81146 | 1.0000 | 62.0 |
| mlfq | 801.5 | 4811.2 | 19000 | 18999.6 | 18046.7 | 111331 | 1.0000 | 29.0 |
| ewma | 799.5 | 6708.3 | 21809 | 21808.7 | 19893.4 | 110892 | 1.0000 | 26.1 |
| ewma_conf | 799.3 | 4799.5 | 22005 | 18002.8 | 22005.4 | 111387 | 1.0000 | 23.2 |
| ewma_conf_slc | 1794.3 | 8797.0 | 47794 | 46815.4 | 42002.1 | 56692 | 1.0000 | 52.0 |
| full | 17.7 | 5809.1 | 39004 | 39003.6 | 33999.3 | 69158 | 0.9999 | 47.0 |
| learned | 10.6 | 34.1 | **89** | 34.6 | 25.1 | 69233 | 0.9999 | 50.0 |
| learned_percpu | 8.8 | **18.2** | 2007 | **18.2** | 2006.6 | 69384 | 0.9998 | 82.2 |

## Workload 3: request/response service

`rr_bench`: a server process answers 64-byte requests over loopback TCP, doing 100 us of
work per request; 4 client threads send 200 requests/s each on a fixed schedule. Latency is
measured from the scheduled send time to the reply.

| config | p50 us | p99 us | max us | hog ops/s | ctx switches | Jain | worst hog wait ms | worst task wait ms | preemptions |
|---|---|---|---|---|---|---|---|---|---|
| eevdf | 120.1 | 3021.9 | 6882 | 63339 | 420149 | 0.9897 | 24.0 | 5.0 | NA |
| scx_simple | 4760.8 | 11099.3 | 13165 | 63505 | 98460 | 1.0000 | 62.0 | 37.2 | NA |
| mlfq | 2091.8 | 7077.4 | 9190 | 63431 | 132119 | 1.0000 | 27.2 | 8.8 | 0 |
| ewma | 1683.1 | 7049.4 | 11122 | 63437 | 137749 | 1.0000 | 25.1 | 10.6 | 0 |
| ewma_conf | 2008.6 | 7033.2 | 7811 | 63468 | 135976 | 1.0000 | 25.0 | 6.9 | 0 |
| ewma_conf_slc | 1972.3 | 16015.5 | 16387 | 63530 | 87905 | 1.0000 | 58.0 | 15.9 | 0 |
| full | 1063.8 | 6965.4 | 14986 | 63461 | 139519 | 0.9998 | 41.0 | 14.8 | 30512 |
| learned | 151.6 | **285.3** | 1783 | 63514 | 143016 | 0.9997 | 41.0 | 2.0 | 30402 |
| learned_percpu | 139.3 | 337.7 | 1802 | 63489 | 143023 | 0.9999 | 82.1 | 2.0 | 29845 |

## What each feature contributed

| step | effect |
|---|---|
| reactive MLFQ | Classifies a steady interactive task correctly, but p99 is bounded by the hogs' slice ends (~11 ms). |
| + EWMA | No measurable gain on its own. |
| + confidence gating | Helps the steady task (p99 10.8 -> 6.1 ms) by rejecting unstable predictions. |
| + predicted slices | Halves context switches (86k -> 49k) but, without preemption, raises latency and the worst hog wait (22 -> 56 ms). |
| + wakeup preemption | The big win for the steady task (p99 -> 180 us). Does not help phase recovery (39 ms) or the request/response clients (7.0 ms). |
| + learned predictor | Fixes phase changes (recovery p99 39 ms -> 35 us) and the request/response service (7.0 ms -> 285 us); steady task 180 -> 34 us. |
| + per-CPU queues | Best tail for the steady task (12 us) and phase recovery (18 us); slightly worse for request/response (285 -> 338 us, all 3 runs); worst hog wait doubles (41 -> 82 ms). |

Hog throughput is the same for every configuration (within 0.5% in each workload). Every
BurstSched configuration is at least as fair between hogs as EEVDF (Jain >= 0.9997 vs
0.990-0.994), but BurstSched's worst single hog wait (41-82 ms) is 2-4x EEVDF's (18-24 ms).

## Why EWMA fails on the request/response service

From the burst logs (`results/bursts_rr_full_*`, run 1):

| task | burst p10 / p50 / p90 | EWMA deviation / prediction | EWMA "confident" | placed by |
|---|---|---|---|---|
| rr_server | 112 / 116 / 121 us | 0.03 | 100% | EWMA prediction |
| rr_bench (clients) | 4 / 8 / 30 us | 0.66 | 1% | reactive fallback |

The clients' bursts are tiny but vary 7x, so their EWMA deviation is large *relative to the
prediction*, and confidence gating rejects it. They are still queued at the right level
(100%), but wakeup preemption is only allowed for prediction-placed tasks, so every client
wakeup waits for a hog's slice to end. The learned predictor's per-leaf error is small for
this pattern, so 99.9% of client bursts are placed by prediction and may preempt.

**Lesson:** confidence should be judged against the distance to the level boundary
(a 4-30 us burst is certainly below 1 ms), not relative to the burst itself.

## How often does prediction miss, and what does it cost?

`analysis/mispredict.py`, `data/final/mispredict.csv`: for every finished burst of the
measured task, the level it was queued at vs the level its real burst called for.

| config | phase task: bursts queued too low | p99 queue wait of those | wakeup task placed correctly | rr tasks placed correctly |
|---|---|---|---|---|
| mlfq | 1.5% | 22 ms | 100% | 100% |
| ewma | 1.5% | 23 ms | 100% | 100% |
| ewma_conf | 1.5% | 22 ms | 100% | 100% |
| ewma_conf_slc | 1.6% | 48 ms | 100% | 100% |
| full | 1.5% | 39 ms | 100% | 100% |
| learned | **0.0%** | none | 100% | 100% |
| learned_percpu | **0.0%** | 5 ms (2 bursts) | 100% | 100% |

- The 1.5% are the ~3 bursts after every CPU phase that reactive placement needs to climb
  from the lowest level back to the top. The learned predictor removes them.
- The steady and request/response tasks are queued at the right level by **every**
  configuration, yet their p99 latency varies up to 1000-fold. Correct placement is not
  enough: what matters is whether the waking task is allowed to preempt.

## Predictor accuracy on held-out data

`predictor/train.py --metrics data/final/predictor_eval.json`: share of next bursts placed in
the correct level, on burst logs the predictor was never trained on.

| held-out log | EWMA, all | learned, all | EWMA, after a behaviour change | learned, after a behaviour change |
|---|---|---|---|---|
| phase workload, partial mode | 74.8% | 97.2% | 0.0% | 68.9% |
| wakeup workload, partial mode | 95.9% | 98.4% | 0.0% | 62.2% |
| phase workload, full mode (all tasks) | 93.1% | 99.1% | 54.4% | 84.6% |
| wakeup workload, full mode (all tasks) | 97.9% | 99.1% | 53.9% | 82.5% |

(The full-mode held-out logs are from the previous final run; the model was trained once on
`scripts/collect_training.sh` data and never retrained.)

## Figures

`python analysis/plots.py` writes `figures/fig1_ablation` ... `fig6_misplacement` (PNG + SVG).

## Known limitations

- **Cold start.** In partial mode (the task joins the scheduler just before its first CPU
  phase) the first CPU-to-interactive switch costs one long queue wait (~42 ms), because there
  is no history (docs/run3_learned.md). In full mode the process's start-up bursts provide
  history and the first switch is usually fast (median 25 us learned, 2 ms per-CPU).
- **Worst-case wait.** 40 ms slices and per-CPU queues raise the longest wait of a CPU-bound
  task to 41-82 ms (EEVDF: 18-24 ms). That is the price of 3-6x fewer context switches.
- **Confidence measure.** Relative EWMA deviation wrongly rejects tiny, variable bursts
  (see the request/response analysis).
- **Workloads.** Three synthetic workloads on one 36-CPU machine with CPU-bound background
  load; no desktop application, compile job or multi-socket machine was measured.
