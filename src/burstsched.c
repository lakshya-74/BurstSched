/* SPDX-License-Identifier: GPL-2.0 */
/* BurstSched user-space loader: loads the scheduler, logs bursts, prints stats. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <libgen.h>
#include <time.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <scx/common.h>
#include "burstsched.h"
#include "burstsched.bpf.skel.h"

const char help_fmt[] =
"BurstSched: burst-predicting MLFQ scheduler (sched_ext).\n"
"\n"
"Usage: %s [options]\n"
"\n"
"  -p          Partial mode: only SCHED_EXT tasks (see build/setext) use BurstSched\n"
"  -e          Place tasks by EWMA burst prediction\n"
"  -l          Prefer learned predictions from user space (falls back to EWMA)\n"
"  -c          Confidence gating: only act on stable predictions\n"
"  -s          Predicted time slices (1.25 x predicted burst)\n"
"  -w          Wakeup preemption by short predicted burst\n"
"  -a MS       Aging threshold in ms (default 100)\n"
"  -t PCT      Confidence threshold: dev < PCT%% of prediction (default 50)\n"
"  -o FILE     Write every finished burst to FILE (CSV)\n"
"  -q          Quiet: don't print stats every second\n"
"  -v          Print libbpf debug messages\n"
"  -h          Display this help and exit\n";

static bool verbose;
static volatile int exit_req;

static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
			   va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sigint_handler(int sig)
{
	exit_req = 1;
}

static int handle_event(void *ctx, void *data, size_t len)
{
	FILE *f = ctx;
	const struct bs_burst_event *e = data;
	char comm[BS_COMM_LEN + 1];
	int i;

	memcpy(comm, e->comm, BS_COMM_LEN);
	comm[BS_COMM_LEN] = '\0';
	for (i = 0; comm[i]; i++)
		if (comm[i] == ',' || comm[i] == '\n')
			comm[i] = '_';

	fprintf(f, "%llu,%u,%s,%llu,%llu,%llu,%llu,%llu,%u,%u\n",
		(unsigned long long)e->ts_ns, e->pid, comm,
		(unsigned long long)e->burst_ns,
		(unsigned long long)e->sleep_ns,
		(unsigned long long)e->pred_ns,
		(unsigned long long)e->dev_ns,
		(unsigned long long)e->wait_ns,
		e->level, e->placed_by);
	return 0;
}

static void read_stats(struct burstsched *skel, __u64 *out)
{
	int nr_cpus = libbpf_num_possible_cpus();
	__u32 idx;

	assert(nr_cpus > 0);
	__u64 cnts[nr_cpus];

	memset(out, 0, sizeof(out[0]) * BS_NR_STATS);
	for (idx = 0; idx < BS_NR_STATS; idx++) {
		int cpu;

		if (bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats), &idx,
					cnts) < 0)
			continue;
		for (cpu = 0; cpu < nr_cpus; cpu++)
			out[idx] += cnts[cpu];
	}
}

int main(int argc, char **argv)
{
	struct burstsched *skel;
	struct bpf_link *link;
	struct ring_buffer *rb = NULL;
	FILE *csv = NULL;
	const char *csv_path = NULL;
	bool partial = false, quiet = false;
	int opt;
	__u64 ecode;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
restart:
	optind = 1;
	skel = SCX_OPS_OPEN(burst_ops, burstsched);

	while ((opt = getopt(argc, argv, "pelcswa:t:o:qvh")) != -1) {
		switch (opt) {
		case 'p':
			partial = true;
			break;
		case 'e':
			skel->rodata->use_ewma = true;
			break;
		case 'l':
			skel->rodata->use_learned = true;
			break;
		case 'c':
			skel->rodata->use_conf = true;
			break;
		case 's':
			skel->rodata->use_slices = true;
			break;
		case 'w':
			skel->rodata->use_preempt = true;
			break;
		case 'a':
			skel->rodata->aging_ns = strtoull(optarg, NULL, 0) * 1000000ULL;
			break;
		case 't':
			skel->rodata->conf_pct = strtoul(optarg, NULL, 0);
			break;
		case 'o':
			csv_path = optarg;
			break;
		case 'q':
			quiet = true;
			break;
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}

	if (partial)
		skel->struct_ops.burst_ops->flags |= SCX_OPS_SWITCH_PARTIAL;

	SCX_OPS_LOAD(skel, burst_ops, burstsched, uei);
	link = SCX_OPS_ATTACH(skel, burst_ops, burstsched);

	printf("burstsched loaded: partial=%d ewma=%d learned=%d conf=%d slices=%d preempt=%d aging=%llums\n",
	       partial, skel->rodata->use_ewma, skel->rodata->use_learned,
	       skel->rodata->use_conf, skel->rodata->use_slices,
	       skel->rodata->use_preempt,
	       (unsigned long long)(skel->rodata->aging_ns / 1000000ULL));

	if (csv_path) {
		csv = fopen(csv_path, "w");
		if (!csv) {
			perror("fopen csv");
			exit_req = 1;
		} else {
			fprintf(csv, "ts_ns,pid,comm,burst_ns,sleep_ns,pred_ns,dev_ns,wait_ns,level,placed_by\n");
			rb = ring_buffer__new(bpf_map__fd(skel->maps.events),
					      handle_event, csv, NULL);
			if (!rb) {
				fprintf(stderr, "failed to create ring buffer\n");
				exit_req = 1;
			}
		}
	}

	time_t last = time(NULL);

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		if (rb)
			ring_buffer__poll(rb, 100);
		else
			usleep(100 * 1000);

		if (!quiet && time(NULL) != last) {
			__u64 s[BS_NR_STATS];

			last = time(NULL);
			read_stats(skel, s);
			printf("local=%llu enq=%llu L0=%llu L1=%llu L2=%llu L3=%llu "
			       "pred=%llu react=%llu aged=%llu preempt=%llu "
			       "maxwait=%.2fms drop=%llu\n",
			       s[BS_ST_LOCAL], s[BS_ST_ENQ], s[BS_ST_L0], s[BS_ST_L1],
			       s[BS_ST_L2], s[BS_ST_L3], s[BS_ST_PRED_PLACED],
			       s[BS_ST_REACT_PLACED], s[BS_ST_AGED], s[BS_ST_PREEMPT],
			       skel->bss->max_wait_ns / 1e6, s[BS_ST_EV_DROPPED]);
			fflush(stdout);
		}
	}

	if (rb)
		ring_buffer__free(rb);
	if (csv)
		fclose(csv);
	rb = NULL;
	csv = NULL;

	{
		__u64 s[BS_NR_STATS];

		read_stats(skel, s);
		printf("final: maxwait_ms=%.3f aged=%llu preempt=%llu pred=%llu react=%llu drop=%llu\n",
		       skel->bss->max_wait_ns / 1e6, s[BS_ST_AGED],
		       s[BS_ST_PREEMPT], s[BS_ST_PRED_PLACED],
		       s[BS_ST_REACT_PLACED], s[BS_ST_EV_DROPPED]);
		fflush(stdout);
	}

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	burstsched__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
