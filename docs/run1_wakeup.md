# Run 1: wakeup workload (lab PC A)

Date: 2026-11-21. Machine: HP Z4 G5, 36 CPUs, kernel 7.0.0-38-generic, partial mode.
Workload: `wakeup_lat -n 3000` (10 ms sleep, 200 us burst) under 72 `stress-ng --cpu` hogs.
3 repetitions, median (min-max). Code version: before the "ongoing burst" fix and
40 ms SLICE_MAX (so hogs never received predictions; predicted slices had no effect).

| config | p50 us | p99 us | max us | hog ops/s | ctx switches | max wait ms |
|---|---|---|---|---|---|---|
| eevdf | 5.3 (4-6) | 4523 (3914-6510) | 9513 (7915-9524) | 63210 | 417757 | NA |
| mlfq | 946 (571-954) | 2961 (2578-3727) | 5570 | 63359 | 258151 | 25.0 (24-30) |
| ewma | 811 (371-980) | 2982 (2376-3810) | 5812 | 63301 | 248868 | 26.0 (25-27) |
| ewma_conf | 464 (148-632) | 3466 (3148-3633) | 6464 | 63309 | 256278 | 25.0 (22-25) |
| ewma_conf_slc | 352 (337-761) | 3758 (3335-4346) | 4762 | 63327 | 249208 | 25.1 (22-26) |
| full (-e -c -s -w) | 8.1 (8-8) | 12.8 (12-16) | 2688 (1302-4659) | 63364 | 246264 | 21.0 (20-25) |

Observations:
- Wakeup preemption by predicted burst is what cuts p99 (~350x vs EEVDF), with no
  hog-throughput loss and ~41% fewer context switches than EEVDF.
- Without preemption, prediction variants are within noise of reactive MLFQ: a steady
  interactive task is already classified correctly by reactive MLFQ.
- Aging bounds worst-case wait at ~20-30 ms.

Final results must all come from ONE machine. Rerun everything on the new PC.
