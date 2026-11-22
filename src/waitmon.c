/* SPDX-License-Identifier: GPL-2.0 */
/*
 * waitmon: measure run-queue wait of every task until SIGINT, then print the
 * worst-case wait per class (all / hogs / workload) and a p99 upper bound.
 * Works with any scheduler (EEVDF, scx_simple, BurstSched).
 *
 * Usage: sudo waitmon        (stop with Ctrl+C / SIGINT)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "waitmon.h"
#include "waitmon.bpf.skel.h"

static volatile int exit_req;

static void on_sig(int sig)
{
	exit_req = 1;
}

static int quiet_print(enum libbpf_print_level level, const char *fmt, va_list ap)
{
	if (level == LIBBPF_DEBUG)
		return 0;
	return vfprintf(stderr, fmt, ap);
}

/* p99 upper bound in ms from a log2-us histogram */
static double p99_ms(const __u64 *hist, __u64 count)
{
	__u64 need, seen = 0;
	int k;

	if (!count)
		return 0;
	need = count - count / 100;
	for (k = 0; k < WM_NSLOT; k++) {
		seen += hist[k];
		if (seen >= need)
			return (double)(1ULL << (k + 1)) / 1000.0;
	}
	return (double)(1ULL << WM_NSLOT) / 1000.0;
}

int main(int argc, char **argv)
{
	struct waitmon *skel;
	int nr_cpus = libbpf_num_possible_cpus(), fd, cpu, c, k;
	struct wm_stat tot[WM_NCLASS], all;
	struct wm_stat *v;

	libbpf_set_print(quiet_print);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	skel = waitmon__open_and_load();
	if (!skel) {
		fprintf(stderr, "waitmon: failed to load BPF program\n");
		return 1;
	}
	if (waitmon__attach(skel)) {
		fprintf(stderr, "waitmon: failed to attach\n");
		waitmon__destroy(skel);
		return 1;
	}
	fprintf(stderr, "waitmon: running, Ctrl+C to stop\n");
	while (!exit_req)
		usleep(100 * 1000);

	v = calloc(nr_cpus, sizeof(*v));
	if (!v)
		return 1;
	memset(tot, 0, sizeof(tot));
	fd = bpf_map__fd(skel->maps.wm_stats);
	for (c = 0; c < WM_NCLASS; c++) {
		__u32 key = c;

		if (bpf_map_lookup_elem(fd, &key, v))
			continue;
		for (cpu = 0; cpu < nr_cpus; cpu++) {
			tot[c].count += v[cpu].count;
			for (k = 0; k < WM_NSLOT; k++)
				tot[c].hist[k] += v[cpu].hist[k];
			if (v[cpu].max_ns > tot[c].max_ns) {
				tot[c].max_ns = v[cpu].max_ns;
				tot[c].max_pid = v[cpu].max_pid;
				memcpy(tot[c].max_comm, v[cpu].max_comm, WM_COMM_LEN);
			}
		}
		tot[c].max_comm[WM_COMM_LEN - 1] = '\0';
	}

	all = tot[0];
	for (c = 1; c < WM_NCLASS; c++) {
		all.count += tot[c].count;
		for (k = 0; k < WM_NSLOT; k++)
			all.hist[k] += tot[c].hist[k];
		if (tot[c].max_ns > all.max_ns) {
			all.max_ns = tot[c].max_ns;
			all.max_pid = tot[c].max_pid;
			memcpy(all.max_comm, tot[c].max_comm, WM_COMM_LEN);
		}
	}

	printf("waitmon: max_all_ms=%.3f max_hog_ms=%.3f max_work_ms=%.3f max_other_ms=%.3f "
	       "p99_all_ms=%.3f p99_hog_ms=%.3f n=%llu worst=%s/%u\n",
	       all.max_ns / 1e6, tot[1].max_ns / 1e6, tot[2].max_ns / 1e6,
	       tot[0].max_ns / 1e6, p99_ms(all.hist, all.count),
	       p99_ms(tot[1].hist, tot[1].count),
	       (unsigned long long)all.count, all.max_comm, all.max_pid);
	fflush(stdout);

	free(v);
	waitmon__destroy(skel);
	return 0;
}
