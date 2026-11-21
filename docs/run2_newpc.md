# Run 2: new lab PC (lab-pc), 2026-11-22

36 CPUs, 72 `stress-ng --cpu` hogs, partial mode, 3 reps, median (min-max).
Code: includes "ongoing burst" evidence (hogs get predictions) and SLICE_MAX = 40 ms.

## Wakeup workload (`wakeup_lat`, 10 ms sleep / 200 us burst)

| config | p50 us | p99 us | hog ops/s | ctx switches | max wait ms |
|---|---|---|---|---|---|
| eevdf | 5.7 | 3617 | 63323 | 363554 | NA |
| mlfq | 435 (115-800) | 4113 | 63447 | 93214 | 20.2 |
| ewma | 648 (158-699) | 5168 | 63460 | 93157 | 21.2 |
| ewma_conf | 100 (66-720) | 3099 | 63552 | 93127 | 21.0 |
| ewma_conf_slc | 1364 | 10802 | 63560 | 50005 | 45.0 |
| full | **8.1** | **11.1** (11-12) | 63558 | **51959** | 43.0 |

## Phase-change workload (`phase_change`, 10 x [2 s CPU, 2 s interactive])

rec_* = first 10 wakeups after each CPU phase (how fast the scheduler re-classifies).

| config | p50 us | p99 us | rec_p99 us | ctx switches | max wait ms |
|---|---|---|---|---|---|
| eevdf | 5.7 | 3796 | **5003** | 475783 | NA |
| mlfq | 801 | 3803 | 18997 | 114869 | 28.7 |
| ewma | 801 | 3804 | 18996 | 114266 | 26.0 |
| ewma_conf | 800 | 3798 | 18993 | 114387 | 24.0 |
| ewma_conf_slc | 1792 | 7793 | 44000 | 59011 | 50.0 |
| full | 9.8 | 5797 | **43594** | 59740 | 50.0 |

## Findings

1. Steady interactive task: full BurstSched p99 11 us vs EEVDF 3617 us (~325x), same
   throughput, **86% fewer context switches** than EEVDF (52k vs 364k).
2. Predicted slices (40 ms for CPU-bound tasks) halve context switches (93k -> 50k) but,
   without preemption, worsen latency and raise worst-case wait 20 -> 45 ms.
   Preemption removes the latency cost; the worst-case-wait cost remains (trade-off).
3. **Phase change is BurstSched's weakness.** After a 2 s CPU phase every BurstSched
   config takes ~19 ms (16 ms slices) or ~44 ms (40 ms slices) for the first wakeup,
   vs 5 ms for EEVDF. Cause: after the CPU burst, the EWMA jumps (~250 ms) and its
   deviation jumps with it, so confidence gating correctly rejects it and falls back to
   the reactive level, which is L3. The task then queues at the tail of L3 behind ~36
   hogs whose slices are 16/40 ms -> wait ~ one slice. Neither the EWMA nor reactive
   MLFQ can know the task became interactive until it has run short bursts again.
4. Next: learned predictor using sleep history (a task that was sleeping regularly
   before the long burst is likely to be interactive again) to fix recovery.
