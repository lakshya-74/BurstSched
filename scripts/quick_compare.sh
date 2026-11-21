#!/bin/bash
# Compare EEVDF / scx_simple / BurstSched configurations, repeated.
# Usage: sudo ./scripts/quick_compare.sh
#        sudo WORKLOAD=phase ./scripts/quick_compare.sh
#        sudo MODE=full ./scripts/quick_compare.sh      (final runs; affects ALL tasks)
# Env:   WORKLOAD wakeup|phase (default wakeup)
#        MODE     partial|full  (default partial: only SCHED_EXT tasks use the BPF scheduler)
#        HOGS (default 2 x nproc), ITERS (wakeup only, default 3000), REPS (default 3)
#        CONFIGS  space-separated subset (default: all available)
#
# Writes results/summary_<workload>[_full].csv and prints a median table.
set -u
cd "$(dirname "$0")/.."

N=$(nproc)
WORKLOAD=${WORKLOAD:-wakeup}
MODE=${MODE:-partial}
HOGS=${HOGS:-$((N * 2))}
ITERS=${ITERS:-3000}
REPS=${REPS:-3}

DEFAULT_CONFIGS="eevdf"
[ "$MODE" = "full" ] && [ -x build/scx_simple ] && DEFAULT_CONFIGS="$DEFAULT_CONFIGS scx_simple"
DEFAULT_CONFIGS="$DEFAULT_CONFIGS mlfq ewma ewma_conf ewma_conf_slc full"
[ -f models/tree.txt ] && DEFAULT_CONFIGS="$DEFAULT_CONFIGS learned learned_percpu"
CONFIGS=${CONFIGS:-$DEFAULT_CONFIGS}

case "$WORKLOAD" in
wakeup) WCMD=(./build/wakeup_lat -n "$ITERS"); WLEN=$(( ITERS / 100 )) ;;
phase)  WCMD=(./build/phase_change -c 10 -C 2000 -I 2000); WLEN=40 ;;
*) echo "unknown WORKLOAD=$WORKLOAD"; exit 1 ;;
esac
case "$MODE" in
partial) PFLAG="-p"; SUF="" ;;
full)    PFLAG="";   SUF="_full" ;;
*) echo "unknown MODE=$MODE"; exit 1 ;;
esac

# hogs must outlast the workload (wakeup_lat runs longer when it is delayed a lot);
# they are stopped with SIGINT right after the workload finishes
DUR=$(( WLEN * 2 + 10 ))
TAG="${WORKLOAD}${SUF}"
OUT=results/summary_${TAG}.csv
mkdir -p results

flags_for() {
	case "$1" in
	mlfq)           echo "" ;;
	ewma)           echo "-e" ;;
	ewma_conf)      echo "-e -c" ;;
	ewma_conf_slc)  echo "-e -c -s" ;;
	full)           echo "-e -c -s -w" ;;
	ewma_preempt)   echo "-e -s -w" ;;
	learned)        echo "-m models/tree.txt -c -s -w" ;;
	learned_noconf) echo "-m models/tree.txt -s -w" ;;
	learned_percpu) echo "-m models/tree.txt -c -s -w -P" ;;
	full_percpu)    echo "-e -c -s -w -P" ;;
	*)              echo "" ;;
	esac
}

NCONF=$(echo $CONFIGS | wc -w)
echo "workload=$WORKLOAD mode=$MODE CPUs=$N hogs=$HOGS reps=$REPS configs=[$CONFIGS] (~$(( (DUR + 8) * NCONF * REPS / 60 )) min)"
echo "config,rep,p50_us,p99_us,max_us,rec_p50_us,rec_p99_us,cold_us,hog_ops_per_s,ctx_switches,jain,max_wait_ms,preempts" > "$OUT"

ctxt() { awk '/^ctxt/ {print $2}' /proc/stat; }
# getf LINE KEY -> value of " KEY=<num>" (space-anchored so p50 != rec_p50)
getf() { echo " $1" | grep -oE "[ (]$2=[0-9.]+" | head -1 | cut -d= -f2; }
# Jain's fairness index over the CPU time received by each hog (1.0 = perfectly fair)
jain() {
	for p in $(pgrep -x stress-ng-cpu); do
		awk '{print $14 + $15}' "/proc/$p/stat" 2>/dev/null
	done | awk '{s += $1; q += $1 * $1; n++} END {if (n && q) printf "%.4f", s * s / (n * q); else printf "NA"}'
}

run() {
	local name=$1 rep=$2
	local S="" log="results/log_${TAG}_${name}_${rep}.txt"
	: > "$log"

	case "$name" in
	eevdf) ;;
	scx_simple)
		if [ "$MODE" != "full" ]; then echo "scx_simple needs MODE=full, skipping"; return; fi
		./build/scx_simple > "$log" 2>&1 &
		S=$!; sleep 2
		if [ "$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)" != "simple" ]; then
			echo "scx_simple failed to load (see $log)"; kill -INT "$S" 2>/dev/null; return
		fi ;;
	*)
		# shellcheck disable=SC2046
		./build/burstsched $PFLAG -q $(flags_for "$name") \
			-o "results/bursts_${TAG}_${name}_${rep}.csv" > "$log" 2>&1 &
		S=$!; sleep 2
		if [ "$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)" != "burstsched" ]; then
			echo "$name: scheduler failed to load (see $log)"; return
		fi ;;
	esac

	local yaml="results/hog_${TAG}_${name}_${rep}.yaml"
	local c0; c0=$(ctxt)
	./build/setext stress-ng --cpu "$HOGS" --timeout "${DUR}s" --quiet \
		--metrics-brief --yaml "$yaml" > /dev/null 2>&1 &
	local H=$!
	sleep 2

	local line fair
	line=$(./build/setext "${WCMD[@]}" -o "results/lat_${TAG}_${name}_${rep}.csv")
	fair=$(jain)                      # hogs are still running here
	kill -INT "$H" 2>/dev/null        # stress-ng stops and writes its metrics
	wait "$H" 2>/dev/null
	local c1; c1=$(ctxt)

	if [ -n "$S" ]; then kill -INT "$S"; wait "$S" 2>/dev/null; fi

	local p50 p99 mx rp50 rp99 cold ops mw pre fin
	p50=$(getf "$line" p50);  p99=$(getf "$line" p99);  mx=$(getf "$line" max)
	rp50=$(getf "$line" rec_p50); rp99=$(getf "$line" rec_p99); cold=$(getf "$line" cold)
	ops=$(grep -m1 "bogo-ops-per-second-real-time" "$yaml" 2>/dev/null | awk '{print $2}')
	fin=$(grep -m1 '^final:' "$log")
	mw=$(getf "$fin" maxwait_ms); pre=$(getf "$fin" preempt)

	echo "$name,$rep,${p50:-NA},${p99:-NA},${mx:-NA},${rp50:-NA},${rp99:-NA},${cold:-NA},${ops:-NA},$((c1 - c0)),${fair:-NA},${mw:-NA},${pre:-NA}" >> "$OUT"
	printf "rep %d %-14s p50=%-8s p99=%-8s rec_p99=%-8s cold=%-9s hog_ops/s=%-7.0f ctxsw=%-7s jain=%-6s maxwait=%sms\n" \
		"$rep" "$name" "${p50:-NA}" "${p99:-NA}" "${rp99:-NA}" "${cold:-NA}" "${ops:-0}" "$((c1 - c0))" "${fair:-NA}" "${mw:-NA}"
	sleep 2
}

for rep in $(seq 1 "$REPS"); do
	for cfg in $CONFIGS; do
		run "$cfg" "$rep"
	done
done

echo
python3 analysis/summarize.py "$OUT"
