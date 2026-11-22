/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __WAITMON_H
#define __WAITMON_H

#define WM_NCLASS	3	/* 0 other, 1 hogs, 2 measured workload */
#define WM_NSLOT	32	/* log2 histogram of wait in microseconds */
#define WM_COMM_LEN	16

struct wm_stat {
	__u64 max_ns;
	__u32 max_pid;
	__u32 pad;
	__u64 count;
	__u64 hist[WM_NSLOT];	/* slot k: [2^k, 2^(k+1)) us; slot 0: < 2 us */
	char max_comm[WM_COMM_LEN];
};

#endif
