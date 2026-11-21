/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BurstSched user-space loader: loads the scheduler, logs bursts, prints stats,
 * and (with -m) runs the learned burst predictor, writing its predictions into
 * the BPF `learned` map.
 */
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
"  -m FILE     Learned model (from predictor/train.py); implies -l\n"
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

/* ---------------- learned predictor (decision tree) ---------------- */

/*
 * Features, computed after each finished burst t of a task. MUST match
 * predictor/train.py:features().
 *   f0 = burst_t, f1 = sleep before burst_t, f2 = EWMA after burst_t,
 *   f3 = burst_{t-1}, f4 = sleep before burst_{t-1}     (all ns, 0 if unknown)
 */
#define NFEAT 5

struct tnode {
	int feat;		/* -1 for a leaf */
	double thr;
	int left, right;
	double val_ns;		/* leaf prediction */
	double dev_ns;		/* leaf expected error */
};

static struct tnode *tree;
static int ntree;

static int load_model(const char *path)
{
	FILE *f = fopen(path, "r");
	int i;

	if (!f) {
		perror("open model");
		return -1;
	}
	if (fscanf(f, "%d", &ntree) != 1 || ntree <= 0 || ntree > 100000) {
		fprintf(stderr, "bad model header\n");
		fclose(f);
		return -1;
	}
	tree = calloc(ntree, sizeof(*tree));
	if (!tree) {
		fclose(f);
		return -1;
	}
	for (i = 0; i < ntree; i++) {
		struct tnode *t = &tree[i];

		if (fscanf(f, "%d %lf %d %d %lf %lf", &t->feat, &t->thr,
			   &t->left, &t->right, &t->val_ns, &t->dev_ns) != 6 ||
		    t->feat >= NFEAT || t->left >= ntree || t->right >= ntree) {
			fprintf(stderr, "bad model node %d\n", i);
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	printf("loaded model %s: %d nodes\n", path, ntree);
	return 0;
}

static void tree_predict(const double *x, double *val, double *dev)
{
	int i = 0, guard = 0;

	while (tree[i].feat >= 0 && tree[i].left >= 0 && guard++ < 64)
		i = x[tree[i].feat] <= tree[i].thr ? tree[i].left : tree[i].right;
	*val = tree[i].val_ns;
	*dev = tree[i].dev_ns;
}

/* per-task history, direct-mapped by pid */
#define HSZ (1 << 16)
struct hist {
	__u32 pid;
	__u32 n;
	__u64 b1, s1, b2, s2, ewma;
};
static struct hist htab[HSZ];

static int learned_fd = -1;
static FILE *csv;
static __u64 nr_learned_updates;

static void learn_event(const struct bs_burst_event *e)
{
	struct hist *h = &htab[e->pid & (HSZ - 1)];
	struct bs_learned_pred lp;
	double x[NFEAT], val, dev;
	__u32 pid = e->pid;

	if (h->pid != e->pid) {
		memset(h, 0, sizeof(*h));
		h->pid = e->pid;
	}
	h->b2 = h->b1;
	h->s2 = h->s1;
	h->b1 = e->burst_ns;
	h->s1 = e->sleep_ns;
	if (h->n == 0)
		h->ewma = h->b1;
	else if (h->b1 > h->ewma)
		h->ewma += (h->b1 - h->ewma) >> 3;
	else
		h->ewma -= (h->ewma - h->b1) >> 3;
	h->n++;

	x[0] = h->b1;
	x[1] = h->s1;
	x[2] = h->ewma;
	x[3] = h->b2;
	x[4] = h->s2;
	tree_predict(x, &val, &dev);

	lp.pred_ns = val < 1000 ? 1000 : (__u64)val;
	lp.dev_ns = dev < 0 ? 0 : (__u64)dev;
	if (!bpf_map_update_elem(learned_fd, &pid, &lp, BPF_ANY))
		nr_learned_updates++;
}

static int handle_event(void *ctx, void *data, size_t len)
{
	const struct bs_burst_event *e = data;

	if (tree && learned_fd >= 0)
		learn_event(e);

	if (csv) {
		char comm[BS_COMM_LEN + 1];
		int i;

		memcpy(comm, e->comm, BS_COMM_LEN);
		comm[BS_COMM_LEN] = '\0';
		for (i = 0; comm[i]; i++)
			if (comm[i] == ',' || comm[i] == '\n')
				comm[i] = '_';

		fprintf(csv, "%llu,%u,%s,%llu,%llu,%llu,%llu,%llu,%u,%u\n",
			(unsigned long long)e->ts_ns, e->pid, comm,
			(unsigned long long)e->burst_ns,
			(unsigned long long)e->sleep_ns,
			(unsigned long long)e->pred_ns,
			(unsigned long long)e->dev_ns,
			(unsigned long long)e->wait_ns,
			e->level, e->placed_by);
	}
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
	const char *csv_path = NULL, *model_path = NULL;
	bool partial = false, quiet = false;
	int opt;
	__u64 ecode;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
restart:
	optind = 1;
	skel = SCX_OPS_OPEN(burst_ops, burstsched);

	while ((opt = getopt(argc, argv, "pelm:cswa:t:o:qvh")) != -1) {
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
		case 'm':
			model_path = optarg;
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

	if (model_path && !tree && load_model(model_path))
		return 1;

	if (partial)
		skel->struct_ops.burst_ops->flags |= SCX_OPS_SWITCH_PARTIAL;

	SCX_OPS_LOAD(skel, burst_ops, burstsched, uei);
	link = SCX_OPS_ATTACH(skel, burst_ops, burstsched);
	learned_fd = bpf_map__fd(skel->maps.learned);
	memset(htab, 0, sizeof(htab));

	printf("burstsched loaded: partial=%d ewma=%d learned=%d model=%s conf=%d slices=%d preempt=%d aging=%llums\n",
	       partial, skel->rodata->use_ewma, skel->rodata->use_learned,
	       model_path ? model_path : "none",
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
		}
	}

	if (csv || tree) {
		rb = ring_buffer__new(bpf_map__fd(skel->maps.events),
				      handle_event, NULL, NULL);
		if (!rb) {
			fprintf(stderr, "failed to create ring buffer\n");
			exit_req = 1;
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
			       "maxwait=%.2fms drop=%llu learned_upd=%llu\n",
			       s[BS_ST_LOCAL], s[BS_ST_ENQ], s[BS_ST_L0], s[BS_ST_L1],
			       s[BS_ST_L2], s[BS_ST_L3], s[BS_ST_PRED_PLACED],
			       s[BS_ST_REACT_PLACED], s[BS_ST_AGED], s[BS_ST_PREEMPT],
			       skel->bss->max_wait_ns / 1e6, s[BS_ST_EV_DROPPED],
			       nr_learned_updates);
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
		printf("final: maxwait_ms=%.3f aged=%llu preempt=%llu pred=%llu react=%llu drop=%llu learned_upd=%llu\n",
		       skel->bss->max_wait_ns / 1e6, s[BS_ST_AGED],
		       s[BS_ST_PREEMPT], s[BS_ST_PRED_PLACED],
		       s[BS_ST_REACT_PLACED], s[BS_ST_EV_DROPPED],
		       nr_learned_updates);
		fflush(stdout);
	}

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	burstsched__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
