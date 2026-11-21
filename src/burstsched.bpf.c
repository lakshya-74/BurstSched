/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BurstSched: a burst-predicting multilevel feedback queue scheduler.
 *
 * Four shared DSQs act as MLFQ levels (0 = highest priority, shortest slice).
 *
 * Reactive mode (no flags): classic MLFQ. A task that uses its whole slice
 * sinks one level; a task that blocks early rises one level.
 *
 * Predictive mode (-e / -l): every finished CPU burst updates a fixed-point
 * EWMA (and its mean deviation). A waking task is placed directly at the level
 * matching its predicted burst instead of waiting to be demoted.
 *
 * Optional features, each a load-time flag for the ablation study:
 *   use_conf    - only trust predictions whose deviation is small
 *   use_slices  - slice length = 1.25 * predicted burst
 *   use_preempt - a short, confident waker kicks a CPU running a long task
 *
 * Aging: a task that waited longer than aging_ns is promoted, and dispatch
 * serves any lower level that has not been served for aging_ns, so no task
 * can starve.
 */
#include <scx/common.bpf.h>
#include "burstsched.h"

char _license[] SEC("license") = "GPL";

/* ---- load-time configuration (set by the loader before load) ---- */
const volatile bool use_ewma;
const volatile bool use_learned;
const volatile bool use_conf;
const volatile bool use_slices;
const volatile bool use_preempt;
const volatile u64  aging_ns   = 100ULL * 1000 * 1000;	/* 100 ms */
const volatile u32  conf_pct   = 50;	/* confident if dev < 50% of pred */
const volatile u32  ewma_shift = 3;	/* alpha = 1/8 */
const volatile u32  min_bursts = 3;	/* history needed before predicting */

UEI_DEFINE(uei);

#define MSEC		(1000ULL * 1000)
#define SLICE_MIN	(1 * MSEC)
#define SLICE_MAX	(40 * MSEC)
#define ONGOING_MIN	(4 * MSEC)

/* exported to user space through .bss */
u64 max_wait_ns;

static u64 last_served[BS_NR_LEVELS];

/* ---- maps ---- */
struct bs_task_ctx {
	u64 run_start;		/* when the task last started running */
	u64 burst_acc;		/* CPU time in the current burst so far */
	u64 sleep_start;	/* when the task last blocked */
	u64 last_sleep;		/* length of the sleep before this burst */
	u64 enq_ts;		/* when the task was queued */
	u64 last_wait;		/* last queue wait */
	u64 pred;		/* EWMA predicted burst */
	u64 dev;		/* EWMA of |burst - pred| */
	u32 nr_bursts;
	u32 level;		/* reactive MLFQ level */
	u32 cur_level;		/* level actually used at last enqueue */
	u32 placed_by;
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct bs_task_ctx);
} task_ctxs SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 16 << 20);
} events SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 65536);
	__type(key, u32);
	__type(value, struct bs_learned_pred);
} learned SEC(".maps");

struct bs_cpu_state {
	u32 level;		/* level of the task running on this CPU */
	u32 pad;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, BS_MAX_CPUS);
	__type(key, u32);
	__type(value, struct bs_cpu_state);
} cpu_states SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, BS_NR_STATS);
} stats SEC(".maps");

/* ---- helpers ---- */
static void stat_inc(u32 idx)
{
	u64 *cnt = bpf_map_lookup_elem(&stats, &idx);
	if (cnt)
		(*cnt)++;
}

static struct bs_task_ctx *lookup_ctx(struct task_struct *p)
{
	return bpf_task_storage_get(&task_ctxs, p, 0, 0);
}

static u64 level_slice(u32 lvl)
{
	switch (lvl) {
	case 0:  return 2 * MSEC;
	case 1:  return 4 * MSEC;
	case 2:  return 8 * MSEC;
	default: return 16 * MSEC;
	}
}

static u32 level_for_burst(u64 burst)
{
	if (burst < 1 * MSEC)
		return 0;
	if (burst < 4 * MSEC)
		return 1;
	if (burst < 12 * MSEC)
		return 2;
	return 3;
}

/*
 * Pick the prediction source. Returns predicted burst (0 = none) and whether
 * it is trusted enough to act on.
 */
static u64 get_pred(struct task_struct *p, struct bs_task_ctx *c, u32 *by,
		    bool *confident)
{
	u64 pred = 0, dev = 0;

	*by = BS_PLACED_REACTIVE;
	*confident = false;

	if (use_learned) {
		u32 pid = p->pid;
		struct bs_learned_pred *lp = bpf_map_lookup_elem(&learned, &pid);

		if (lp && lp->pred_ns) {
			pred = lp->pred_ns;
			dev = lp->dev_ns;
			*by = BS_PLACED_LEARNED;
		}
	}

	if (!pred && (use_ewma || use_learned) && c->nr_bursts >= min_bursts) {
		pred = c->pred;
		dev = c->dev;
		*by = BS_PLACED_EWMA;
	}

	/*
	 * A burst still in progress is hard evidence: a task that has already
	 * run ONGOING_MIN without blocking is CPU-bound right now, even if it
	 * has never blocked (so has no history) or its history says otherwise.
	 */
	if ((use_ewma || use_learned) && c->burst_acc >= ONGOING_MIN &&
	    c->burst_acc > pred) {
		pred = c->burst_acc;
		dev = 0;
		if (*by == BS_PLACED_REACTIVE)
			*by = BS_PLACED_EWMA;
	}

	if (!pred) {
		*by = BS_PLACED_REACTIVE;
		return 0;
	}

	*confident = !use_conf || dev * 100 < pred * conf_pct;
	return pred;
}

/* Decide queue level and slice for a task about to be queued. */
static void place(struct task_struct *p, struct bs_task_ctx *c, u32 *lvl,
		  u64 *slice)
{
	u32 by;
	bool confident;
	u64 pred = get_pred(p, c, &by, &confident);
	u64 eff = 0;

	if (pred && confident) {
		/* the burst in progress is a lower bound on the real burst */
		eff = pred > c->burst_acc ? pred : c->burst_acc;
		*lvl = level_for_burst(eff);
		c->placed_by = by;
	} else {
		*lvl = c->level;
		c->placed_by = BS_PLACED_REACTIVE;
	}

	if (*lvl >= BS_NR_LEVELS)
		*lvl = BS_NR_LEVELS - 1;

	if (use_slices && c->placed_by != BS_PLACED_REACTIVE) {
		u64 s = eff * 5 / 4;

		if (s < SLICE_MIN)
			s = SLICE_MIN;
		if (s > SLICE_MAX)
			s = SLICE_MAX;
		*slice = s;
	} else {
		*slice = level_slice(*lvl);
	}

	c->cur_level = *lvl;
	c->enq_ts = bpf_ktime_get_ns();
	stat_inc(c->placed_by != BS_PLACED_REACTIVE ? BS_ST_PRED_PLACED
						 : BS_ST_REACT_PLACED);
}

/* A short, confident waker preempts a CPU that is running a long task. */
static void maybe_preempt(struct task_struct *p)
{
	s32 cpu = scx_bpf_task_cpu(p);
	u32 key;
	struct bs_cpu_state *cs;

	if (cpu < 0 || cpu >= BS_MAX_CPUS)
		return;
	key = cpu;
	cs = bpf_map_lookup_elem(&cpu_states, &key);
	if (cs && cs->level >= 2) {
		scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
		stat_inc(BS_ST_PREEMPT);
	}
}

/* Called when a task blocks: its CPU burst is complete. */
static void burst_done(struct task_struct *p, struct bs_task_ctx *c, u64 now)
{
	u64 burst = c->burst_acc;
	u64 old_pred = c->pred, old_dev = c->dev, err;
	struct bs_burst_event *e;

	/* reactive MLFQ: blocked well before using its slice -> rise */
	if (c->level > 0 && burst < level_slice(c->level) / 2)
		c->level--;

	/* fixed-point EWMA: pred += (burst - pred) / 2^shift */
	if (c->nr_bursts == 0) {
		c->pred = burst;
		c->dev = burst / 2;
	} else {
		err = burst > old_pred ? burst - old_pred : old_pred - burst;
		if (burst > old_pred)
			c->pred = old_pred + ((burst - old_pred) >> ewma_shift);
		else
			c->pred = old_pred - ((old_pred - burst) >> ewma_shift);
		if (err > old_dev)
			c->dev = old_dev + ((err - old_dev) >> ewma_shift);
		else
			c->dev = old_dev - ((old_dev - err) >> ewma_shift);
	}
	if (c->nr_bursts < 0xffffffff)
		c->nr_bursts++;

	e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
	if (e) {
		e->ts_ns = now;
		e->burst_ns = burst;
		e->sleep_ns = c->last_sleep;
		e->pred_ns = old_pred;
		e->dev_ns = old_dev;
		e->wait_ns = c->last_wait;
		e->pid = p->pid;
		e->level = c->cur_level;
		e->placed_by = c->placed_by;
		e->pad = 0;
		bpf_probe_read_kernel_str(e->comm, sizeof(e->comm), p->comm);
		bpf_ringbuf_submit(e, 0);
	} else {
		stat_inc(BS_ST_EV_DROPPED);
	}

	c->burst_acc = 0;
	c->sleep_start = now;
}

/* ---- sched_ext callbacks ---- */
s32 BPF_STRUCT_OPS(burst_select_cpu, struct task_struct *p, s32 prev_cpu,
		   u64 wake_flags)
{
	bool is_idle = false;
	s32 cpu;

	cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
	if (is_idle) {
		struct bs_task_ctx *c = lookup_ctx(p);
		u32 lvl = 0;
		u64 slice = SCX_SLICE_DFL;

		if (c)
			place(p, c, &lvl, &slice);
		stat_inc(BS_ST_LOCAL);
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice, 0);
	}
	return cpu;
}

void BPF_STRUCT_OPS(burst_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct bs_task_ctx *c = lookup_ctx(p);
	u32 lvl = BS_NR_LEVELS - 1;
	u64 slice = SCX_SLICE_DFL;

	stat_inc(BS_ST_ENQ);
	if (c)
		place(p, c, &lvl, &slice);
	if (lvl >= BS_NR_LEVELS)
		lvl = BS_NR_LEVELS - 1;

	scx_bpf_dsq_insert(p, lvl, slice, enq_flags);

	if (use_preempt && c && lvl == 0 && c->placed_by != BS_PLACED_REACTIVE &&
	    (enq_flags & SCX_ENQ_WAKEUP))
		maybe_preempt(p);
}

void BPF_STRUCT_OPS(burst_dispatch, s32 cpu, struct task_struct *prev)
{
	u64 now = bpf_ktime_get_ns();
	u32 i;

	/* aging: serve a lower level that has been starved for aging_ns */
	for (i = BS_NR_LEVELS - 1; i > 0; i--) {
		if (scx_bpf_dsq_nr_queued(i) <= 0) {
			last_served[i] = now;
			continue;
		}
		if (now - last_served[i] > aging_ns &&
		    scx_bpf_dsq_move_to_local(i)) {
			last_served[i] = now;
			stat_inc(BS_ST_L0 + i);
			return;
		}
	}

	/* normal: highest non-empty level first */
	for (i = 0; i < BS_NR_LEVELS; i++) {
		if (scx_bpf_dsq_move_to_local(i)) {
			last_served[i] = now;
			stat_inc(BS_ST_L0 + i);
			return;
		}
	}
}

void BPF_STRUCT_OPS(burst_runnable, struct task_struct *p, u64 enq_flags)
{
	struct bs_task_ctx *c = lookup_ctx(p);

	if (c && c->sleep_start) {
		c->last_sleep = bpf_ktime_get_ns() - c->sleep_start;
		c->sleep_start = 0;
	}
}

void BPF_STRUCT_OPS(burst_running, struct task_struct *p)
{
	struct bs_task_ctx *c = lookup_ctx(p);
	u64 now = bpf_ktime_get_ns();
	u32 key = bpf_get_smp_processor_id();
	struct bs_cpu_state *cs;

	if (!c)
		return;

	c->run_start = now;
	if (c->enq_ts) {
		u64 w = now - c->enq_ts;

		c->last_wait = w;
		c->enq_ts = 0;
		if (w > max_wait_ns)
			max_wait_ns = w;
		/* aging: a task that waited too long rises one level */
		if (w > aging_ns && c->level > 0) {
			c->level--;
			stat_inc(BS_ST_AGED);
		}
	}

	cs = bpf_map_lookup_elem(&cpu_states, &key);
	if (cs)
		cs->level = c->cur_level;
}

void BPF_STRUCT_OPS(burst_stopping, struct task_struct *p, bool runnable)
{
	struct bs_task_ctx *c = lookup_ctx(p);
	u64 now = bpf_ktime_get_ns();
	u32 key = bpf_get_smp_processor_id();
	struct bs_cpu_state *cs = bpf_map_lookup_elem(&cpu_states, &key);

	if (cs)
		cs->level = 0;
	if (!c || !c->run_start)
		return;

	c->burst_acc += now - c->run_start;
	c->run_start = 0;

	if (runnable) {
		/* preempted or slice expired; full slice used -> sink */
		if (p->scx.slice == 0 && c->level < BS_NR_LEVELS - 1)
			c->level++;
		return;
	}

	burst_done(p, c, now);
}

s32 BPF_STRUCT_OPS(burst_init_task, struct task_struct *p,
		   struct scx_init_task_args *args)
{
	if (!bpf_task_storage_get(&task_ctxs, p, 0,
				  BPF_LOCAL_STORAGE_GET_F_CREATE))
		return -ENOMEM;
	return 0;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(burst_init)
{
	u32 i;
	s32 ret;

	for (i = 0; i < BS_NR_LEVELS; i++) {
		ret = scx_bpf_create_dsq(i, -1);
		if (ret) {
			scx_bpf_error("failed to create DSQ %u (%d)", i, ret);
			return ret;
		}
	}
	return 0;
}

void BPF_STRUCT_OPS(burst_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(burst_ops,
	       .select_cpu	= (void *)burst_select_cpu,
	       .enqueue		= (void *)burst_enqueue,
	       .dispatch	= (void *)burst_dispatch,
	       .runnable	= (void *)burst_runnable,
	       .running		= (void *)burst_running,
	       .stopping	= (void *)burst_stopping,
	       .init_task	= (void *)burst_init_task,
	       .init		= (void *)burst_init,
	       .exit		= (void *)burst_exit,
	       .name		= "burstsched");
