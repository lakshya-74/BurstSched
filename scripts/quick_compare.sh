#!/bin/bash
# Compare EEVDF vs BurstSched configurations (partial mode), repeated.
# Usage: sudo ./scripts/quick_compare.sh
#        sudo WORKLOAD=phase ./scripts/quick_compare.sh
# Env:   WORKLOAD wakeup|phase (default wakeup), HOGS (default 2 x nproc),
#        ITERS (wakeup only, default 3000), REPS (default 3),
#        CONFIGS (space-separated subset, default all)
#
# Writes results/summary_<workload>.csv and prints a median table.
set -u
cd "$(dirname "$0")/.."

N=$(nproc)
WORKLOAD=${WORKLOAD:-wakeup}
HOGS=${HOGS:-$((N * 2))}
ITERS=${ITERS:-3000}
REPS=${REPS:-3}
CONFIGS=${CONFIGS:-"eevdf mlfq ewma ewma_conf ewma_conf_slc full"}

case "$WORKLOAD" in
wakeup) WCMD=(./build/wakeup_lat -n "$ITERS"); WLEN=$(( ITERS / 100 )) ;;
phase)  WCMD=(./build/phase_change -c 10 -C 2000 -I 2000); WLEN=40 ;;
*) echo "unknown WORKLOAD=$WORKLOAD"; exit 1 ;;
esac
DUR=$(( WLEN + 5 ))
OUT=results/summary_${WORKLOAD}.csv
mkdir -p results

flags_for() {
	case "$1" in
	mlfq)          echo "" ;;
	ewma)          echo "-e" ;;
	ewma_conf)     echo "-e -c" ;;
	ewma_conf_slc) echo "-e -c -s" ;;
	full)          echo "-e -c -s -w" ;;
	ewma_preempt)  echo "-e -s -w" ;;
	learned)       echo "-l -c -s -w" ;;
	*)             echo "" ;;
	esac
}

NCONF=$(echo $CONFIGS | wc -w)
echo "workload=$WORKLOAD CPUs=$N hogs=$HOGS reps=$REPS configs=[$CONFIGS] (~$(( (DUR + 8) * NCONF * REPS / 60 )) min)"
echo "config,rep,p50_us,p99_us,max_us,rec_p50_us,rec_p99_us,hog_ops_per_s,ctx_switches,max_wait_ms,preempts" > "$OUT"

ctxt() { awk '/^ctxt/ {print $2}' /proc/stat; }
# getf LINE KEY -> value of " KEY=<num>" (space-anchored so p50 != rec_p50)
getf() { echo " $1" | grep -oE "[ (]$2=[0-9.]+" | head -1 | cut -d= -f2; }

run() {
	local name=$1 rep=$2
	local S="" log="results/log_${WORKLOAD}_${name}_${rep}.txt"
	: > "$log"

	if [ "$name" != "eevdf" ]; then
		# shellcheck disable=SC2046
		./build/burstsched -p -q $(flags_for "$name") \
			-o "results/bursts_${WORKLOAD}_${name}_${rep}.csv" > "$log" 2>&1 &
		S=$!
		sleep 2
		if [ "$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)" != "burstsched" ]; then
			echo "$name: scheduler failed to load (see $log)"; return
		fi
	fi

	local yaml="results/hog_${WORKLOAD}_${name}_${rep}.yaml"
	local c0; c0=$(ctxt)
	./build/setext stress-ng --cpu "$HOGS" --timeout "${DUR}s" --quiet \
		--metrics-brief --yaml "$yaml" > /dev/null 2>&1 &
	local H=$!
	sleep 2

	local line
	line=$(./build/setext "${WCMD[@]}" -o "results/lat_${WORKLOAD}_${name}_${rep}.csv")
	wait "$H" 2>/dev/null
	local c1; c1=$(ctxt)

	if [ -n "$S" ]; then kill -INT "$S"; wait "$S" 2>/dev/null; fi

	local p50 p99 mx rp50 rp99 ops mw pre
	p50=$(getf "$line" p50);  p99=$(getf "$line" p99);  mx=$(getf "$line" max)
	rp50=$(getf "$line" rec_p50); rp99=$(getf "$line" rec_p99)
	ops=$(grep -m1 "bogo-ops-per-second-real-time" "$yaml" 2>/dev/null | awk '{print $2}')
	mw=$(getf "$(grep -m1 '^final:' "$log")" maxwait_ms)
	pre=$(getf "$(grep -m1 '^final:' "$log")" preempt)

	echo "$name,$rep,${p50:-NA},${p99:-NA},${mx:-NA},${rp50:-NA},${rp99:-NA},${ops:-NA},$((c1 - c0)),${mw:-NA},${pre:-NA}" >> "$OUT"
	printf "rep %d %-14s p50=%-8s p99=%-8s rec_p99=%-8s hog_ops/s=%-10.0f ctxsw=%-8s maxwait=%sms\n" \
		"$rep" "$name" "${p50:-NA}" "${p99:-NA}" "${rp99:-NA}" "${ops:-0}" "$((c1 - c0))" "${mw:-NA}"
	sleep 2
}

for rep in $(seq 1 "$REPS"); do
	for cfg in $CONFIGS; do
		run "$cfg" "$rep"
	done
done

echo
python3 analysis/summarize.py "$OUT"
