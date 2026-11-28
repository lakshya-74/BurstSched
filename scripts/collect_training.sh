#!/bin/bash
# Collect burst traces for training the learned predictor.
# Uses workloads with DIFFERENT parameters from the test workloads
# (test = phase_change -C 2000 -I 2000 -i 10000 -w 200, wakeup_lat -i 10000 -w 200),
# so evaluation on the test runs is on unseen behaviour.
#
# Usage: sudo ./scripts/collect_training.sh        (REPS=2 by default, ~1.5 min)
set -u
cd "$(dirname "$0")/.."
REPS=${REPS:-2}
N=$(nproc)
mkdir -p results

for rep in $(seq 1 "$REPS"); do
	out="results/train_${rep}.csv"
	echo "collect rep $rep -> $out"
	./build/burstsched -p -q -o "$out" > "results/log_train_${rep}.txt" 2>&1 &
	S=$!
	sleep 2
	if [ "$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)" != "burstsched" ]; then
		echo "scheduler failed to load"; cat "results/log_train_${rep}.txt"; exit 1
	fi

	X=./build/setext
	$X stress-ng --cpu "$N" --timeout 35s --quiet &                # contention
	$X stress-ng --cpu 4 --cpu-load 30 --timeout 30s --quiet &     # on/off CPU
	$X stress-ng --pipe 2 --timeout 30s --quiet &                  # short I/O-like bursts
	$X ./build/phase_change -c 6 -C 800  -I 1500 -i 5000  -w 100  > /dev/null &
	$X ./build/phase_change -c 4 -C 3000 -I 1200 -i 20000 -w 1000 > /dev/null &
	$X ./build/phase_change -c 8 -C 300  -I 600  -i 2000  -w 50   > /dev/null &
	$X ./build/wakeup_lat -n 6000 -i 3000  -w 50   > /dev/null &
	$X ./build/wakeup_lat -n 1200 -i 20000 -w 2000 > /dev/null &
	$X ./build/wakeup_lat -n 500  -i 50000 -w 8000 > /dev/null &
	wait $(jobs -p | grep -v "^$S$") 2>/dev/null

	kill -INT "$S"; wait "$S" 2>/dev/null
	echo "  $(($(wc -l < "$out") - 1)) bursts"
	sleep 2
done
