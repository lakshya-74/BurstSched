# Run 3: learned predictor (lab PC B), 2026-11-22

36 CPUs, 72 hogs, partial mode, 3 reps, median (min-max).
mlfq/ewma/ewma_conf/ewma_conf_slc rows of the recovery table come from run 2 (same
machine, same scheduler logic); eevdf/full/learned are from run 3.

## Predictor accuracy (offline, held-out workloads never seen in training)

Training: scripts/collect_training.sh (2 reps, 44.8k samples after capping each task
at 3000). Test: burst logs of the standard phase and wakeup workloads.

| held-out test | EWMA level-acc | learned level-acc |
|---|---|---|
| phase workload, all bursts | 74.8% | **97.2%** |
| phase workload, phase-change bursts | 0.0% | **68.9%** |
| wakeup workload, all bursts | 95.9% | **98.4%** |
| bursts right after each CPU phase of phase_change | 0 / 9 | **9 / 9** |

v1 of the learned predictor (regression tree on absolute burst sizes) FAILED on the
held-out data (32.8% level accuracy on wakeup): a tree can only output burst sizes it
saw in training (50us, 1ms, 2ms, 8ms), never the test's 206us. v2 is scale-free: a
classifier that chooses between the task's own last burst / EWMA / burst-before-last
from log-ratio features. Feature importance: b1/ewma 0.38, b1/b2 0.32, s1/b1 0.18.

## Phase-change workload

| config | p50 us | p99 us | ctx switches |
|---|---|---|---|
| eevdf | 5.8 | 3647 | 465451 |
| full (EWMA) | 9.8 | 5794 | 59533 |
| **learned** | **8.8** | **13.2** | 60193 |

Recovery after each CPU phase (from per-wakeup logs, analysis/phase_recovery.py):

| config | warm rec p50 us | warm rec p99 us | warm rec max us | cold start ms |
|---|---|---|---|---|
| eevdf | 6.2 | 2794 | 4000 | ~0 |
| mlfq | 801 | 18089 | 18804 | 19.0 |
| ewma | 801 | 17996 | 17999 | 19.0 |
| ewma_conf | 801 | 18093 | 18798 | 17.4 |
| ewma_conf_slc | 803 | 42826 | 43000 | 43.0 |
| full | 801 | 41704 | 42588 | 42.8 |
| **learned** | **9.9** | **18.7** | **19.1** | 41.7 |

warm = first 10 wakeups after each CPU phase in cycles 2-10 (task has history).
cold = the very first wakeup of the run (task has never slept before).

## Wakeup workload

| config | p50 us | p99 us | hog ops/s | ctx switches | max wait ms |
|---|---|---|---|---|---|
| eevdf | 5.9 | 3477 | 63320 | 362639 | NA |
| full | 8.6 | 11.7 | 63558 | 52253 | 42.0 |
| learned | 8.8 | 12.2 | 63554 | 52073 | 42.2 |

## Findings

1. The learned predictor fixes warm phase-change recovery: worst recovery wakeup
   19 us vs 42.6 ms (EWMA) and 4.0 ms (EEVDF). Overall phase p99: 13 us vs 3.6 ms EEVDF.
2. It costs nothing on the steady workload (12.2 vs 11.7 us p99, within noise).
3. Limitation: cold start. The first ever CPU->interactive switch cannot be predicted
   from history; BurstSched pays one L3 queue wait (~42 ms with 40 ms slices) there,
   while EEVDF (no classification) does not.
