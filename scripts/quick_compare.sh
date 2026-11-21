#!/bin/bash
# Compare EEVDF vs BurstSched configurations (partial mode), repeated.
# Usage: sudo ./scripts/quick_compare.sh
# Env:   HOGS (default 2 x nproc), ITERS (default 3000), REPS (default 3)
#
# Writes results/summary.csv; summarize with: python3 analysis/summarize.py
set -u
cd "$(dirname "$0")/.."

N=$(nproc)
HOGS=${HOGS:-$((N * 2))}
ITERS=${ITERS:-3000}
REPS=${REPS:-3}
DUR=$(( ITERS / 100 + 5 ))      # wakeup_lat runs ITERS x 10ms; hogs a bit longer
OUT=results/summary.csv
mkdir -p results

echo "CPUs=$N hogs=$HOGS iters=$ITERS reps=$REPS (~$(( (DUR + 8) * 6 * REPS / 60 )) min)"
echo "config,rep,p50_us,p90_us,p99_us,max_us,hog_ops_per_s,ctx_switches,max_wait_ms,preempts" > "$OUT"

ctxt() { awk '/^ctxt/ {print $2}' /proc/stat; }

run() {
	local name=$1 rep=$2; shift 2
	local S="" log="results/log_${name}_${rep}.txt"
	: > "$log"

	if [ "$name" != "eevdf" ]; then
		./build/burstsched -p -q "$@" -o "results/q_${name}_${rep}.csv" > "$log" 2>&1 &
		S=$!
		sleep 2
		if [ "$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)" != "burstsched" ]; then
			echo "$name: scheduler failed to load (see $log)"; return
		fi
	fi

	local yaml="results/hog_${name}_${rep}.yaml"
	local c0; c0=$(ctxt)
	./build/setext stress-ng --cpu "$HOGS" --timeout "${DUR}s" --quiet \
		--metrics-brief --yaml "$yaml" > /dev/null 2>&1 &
	local H=$!
	sleep 2

	local line
	line=$(./build/setext ./build/wakeup_lat -n "$ITERS" -o "results/lat_${name}_${rep}.csv")
	wait "$H" 2>/dev/null
	local c1; c1=$(ctxt)

	if [ -n "$S" ]; then kill -INT "$S"; wait "$S" 2>/dev/null; fi

	local p50 p90 p99 mx ops mw pre
	p50=$(echo "$line" | sed -E 's/.*p50=([0-9.]+).*/\1/')
	p90=$(echo "$line" | sed -E 's/.*p90=([0-9.]+).*/\1/')
	p99=$(echo "$line" | sed -E 's/.*p99=([0-9.]+).*/\1/')
	mx=$(echo "$line"  | sed -E 's/.*max=([0-9.]+).*/\1/')
	ops=$(grep -m1 "bogo-ops-per-second-real-time" "$yaml" 2>/dev/null | awk '{print $2}')
	mw=$(grep -m1 "^final:" "$log" | sed -E 's/.*maxwait_ms=([0-9.]+).*/\1/')
	pre=$(grep -m1 "^final:" "$log" | sed -E 's/.*preempt=([0-9]+).*/\1/')

	echo "$name,$rep,$p50,$p90,$p99,$mx,${ops:-NA},$((c1 - c0)),${mw:-NA},${pre:-NA}" >> "$OUT"
	printf "rep %d %-14s p50=%-8s p99=%-8s hog_ops/s=%-10s ctxsw=%-9s maxwait=%sms\n" \
		"$rep" "$name" "$p50" "$p99" "${ops:-NA}" "$((c1 - c0))" "${mw:-NA}"
	sleep 2
}

for rep in $(seq 1 "$REPS"); do
	run eevdf         "$rep"                 # default Linux scheduler
	run mlfq          "$rep"                 # reactive MLFQ, no prediction
	run ewma          "$rep" -e
	run ewma_conf     "$rep" -e -c
	run ewma_conf_slc "$rep" -e -c -s
	run full          "$rep" -e -c -s -w
done

echo
python3 analysis/summarize.py "$OUT"
