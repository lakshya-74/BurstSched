#!/bin/bash
# Final ablation: every configuration, both workloads, FULL mode, REPS reps (default 3).
# ~45 min. Run detached so an SSH drop doesn't kill it:
#   sudo -v && sudo nohup ./scripts/final_ablation.sh > results/final_run.out 2>&1 &
#   tail -f results/final_run.out
set -u
cd "$(dirname "$0")/.."
REPS=${REPS:-3}
CONFIGS=${CONFIGS:-"eevdf scx_simple mlfq ewma ewma_conf ewma_conf_slc full learned learned_percpu"}
mkdir -p results

echo "final ablation started $(date)  reps=$REPS"
echo "logged-in sessions:"; who
uname -r; nproc

MODE=full REPS=$REPS CONFIGS="$CONFIGS" WORKLOAD=wakeup ./scripts/quick_compare.sh
MODE=full REPS=$REPS CONFIGS="$CONFIGS" WORKLOAD=phase  ./scripts/quick_compare.sh

echo "final ablation finished $(date)"
echo "summaries: results/summary_wakeup_full.csv results/summary_phase_full.csv"
