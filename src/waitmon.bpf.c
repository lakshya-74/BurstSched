/* SPDX-License-Identifier: GPL-2.0 */
/*
 * waitmon: scheduler-independent run-queue wait monitor.
 *
 * Uses the core scheduler tracepoints, so it measures EEVDF, scx_simple and
 * BurstSched the same way. A task starts waiting when it is woken up or when it
 * is switched out while still runnable (preempted / slice expired); it stops
 * waiting when it is switched in. Records the maximum wait and a log2
 * histogram per task class: 0 = other, 1 = CPU hogs (stress-ng*),
 * 2 = the measured workload (wakeup_lat, phase_change, rr_bench).
 */
#include <scx/common.bpf.h>
#include "waitmon.h"

char _license[] SEC("license") = "GPL";

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1 << 18);
	__type(key, u32);
	__type(value, u64);
} wm_start SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, WM_NCLASS);
	__type(key, u32);
	__type(value, struct wm_stat);
} wm_stats SEC(".maps");

static __always_inline u32 classify(const char *c)
{
	if (c[0] == 's' && c[1] == 't' && c[2] == 'r' && c[3] == 'e' &&
	    c[4] == 's' && c[5] == 's' && c[6] == '-' && c[7] == 'n' && c[8] == 'g')
		return 1;
	if ((c[0] == 'w' && c[1] == 'a' && c[2] == 'k' && c[3] == 'e') ||
	    (c[0] == 'p' && c[1] == 'h' && c[2] == 'a' && c[3] == 's') ||
	    (c[0] == 'r' && c[1] == 'r' && c[2] == '_'))
		return 2;
	return 0;
}

static __always_inline void mark(struct task_struct *p)
{
	u32 pid = p->pid;
	u64 ts;

	if (!pid)
		return;
	ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&wm_start, &pid, &ts, BPF_ANY);
}

static __always_inline void record(struct task_struct *p, u64 d)
{
	char comm[WM_COMM_LEN] = {};
	struct wm_stat *s;
	u64 us = d / 1000;
	u32 cls, slot = 0;
	int i;

	bpf_probe_read_kernel_str(comm, sizeof(comm), p->comm);
	cls = classify(comm);
	s = bpf_map_lookup_elem(&wm_stats, &cls);
	if (!s)
		return;

	for (i = 0; i < WM_NSLOT - 1; i++) {
		if (us < 2)
			break;
		us >>= 1;
		slot++;
	}
	if (slot >= WM_NSLOT)
		slot = WM_NSLOT - 1;
	s->hist[slot]++;
	s->count++;
	if (d > s->max_ns) {
		s->max_ns = d;
		s->max_pid = p->pid;
		__builtin_memcpy(s->max_comm, comm, sizeof(comm));
	}
}

SEC("tp_btf/sched_wakeup")
int BPF_PROG(wm_wakeup, struct task_struct *p)
{
	mark(p);
	return 0;
}

SEC("tp_btf/sched_wakeup_new")
int BPF_PROG(wm_wakeup_new, struct task_struct *p)
{
	mark(p);
	return 0;
}

SEC("tp_btf/sched_switch")
int BPF_PROG(wm_switch, bool preempt, struct task_struct *prev,
	     struct task_struct *next, unsigned int prev_state)
{
	u32 pid;
	u64 *tsp, d;

	/* still runnable when switched out (TASK_RUNNING == 0): it waits again */
	if (prev_state == 0)
		mark(prev);

	pid = next->pid;
	if (!pid)
		return 0;
	tsp = bpf_map_lookup_elem(&wm_start, &pid);
	if (!tsp)
		return 0;
	d = bpf_ktime_get_ns() - *tsp;
	bpf_map_delete_elem(&wm_start, &pid);
	record(next, d);
	return 0;
}
