#!/bin/bash
# Final ablation: every configuration, all workloads, FULL mode, REPS reps (default 3).
# ~60 min. Run detached so an SSH drop doesn't kill it (redirect as root):
#   sudo -v && sudo sh -c 'nohup ./scripts/final_ablation.sh > results/final_run.out 2>&1' &
#   tail -F results/final_run.out
set -u
cd "$(dirname "$0")/.."
REPS=${REPS:-3}
CONFIGS=${CONFIGS:-"eevdf scx_simple mlfq ewma ewma_conf ewma_conf_slc full learned learned_percpu"}
WORKLOADS=${WORKLOADS:-"wakeup phase rr"}
mkdir -p results

echo "final ablation started $(date)  reps=$REPS workloads=[$WORKLOADS]"
echo "kernel $(uname -r), $(nproc) CPUs"

for w in $WORKLOADS; do
	MODE=full REPS=$REPS CONFIGS="$CONFIGS" WORKLOAD=$w ./scripts/quick_compare.sh
done

echo "final ablation finished $(date)"
for w in $WORKLOADS; do echo "summary: results/summary_${w}_full.csv"; done
