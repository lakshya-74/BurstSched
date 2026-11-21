/* SPDX-License-Identifier: GPL-2.0 */
/* Shared definitions between the BPF scheduler and the user-space loader. */
#ifndef __BURSTSCHED_H
#define __BURSTSCHED_H

#define BS_NR_LEVELS	4	/* MLFQ levels; DSQ id == level */
#define BS_MAX_CPUS	1024
#define BS_COMM_LEN	16

/* How a task was placed at its last enqueue */
enum bs_placed_by {
	BS_PLACED_REACTIVE	= 0,	/* classic MLFQ level */
	BS_PLACED_EWMA	= 1,	/* in-kernel EWMA prediction */
	BS_PLACED_LEARNED	= 2,	/* user-space learned prediction */
};

/* One record per finished CPU burst (task ran, then blocked) */
struct bs_burst_event {
	__u64 ts_ns;
	__u64 burst_ns;		/* CPU time of this burst */
	__u64 sleep_ns;		/* sleep that preceded this burst */
	__u64 pred_ns;		/* EWMA prediction made BEFORE this burst */
	__u64 dev_ns;		/* EWMA deviation BEFORE this burst */
	__u64 wait_ns;		/* last run-queue wait */
	__u32 pid;
	__u32 level;		/* queue level used at last enqueue */
	__u32 placed_by;	/* enum bs_placed_by */
	__u32 pad;
	char comm[BS_COMM_LEN];
};

/* Written by user space (learned predictor), keyed by pid */
struct bs_learned_pred {
	__u64 pred_ns;		/* predicted next burst */
	__u64 dev_ns;		/* expected error, used for confidence gating */
};

enum bs_stat_idx {
	BS_ST_LOCAL,		/* direct-dispatched to an idle CPU */
	BS_ST_ENQ,			/* went through enqueue */
	BS_ST_L0, BS_ST_L1, BS_ST_L2, BS_ST_L3,	/* dispatched from each level */
	BS_ST_AGED,		/* aging promotions */
	BS_ST_PREEMPT,		/* wakeup preemption kicks */
	BS_ST_PRED_PLACED,		/* placed by prediction */
	BS_ST_REACT_PLACED,	/* placed reactively */
	BS_ST_EV_DROPPED,		/* ring buffer full */
	BS_NR_STATS,
};

#endif /* __BURSTSCHED_H */
