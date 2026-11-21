/*
 * phase_change: a task that alternates between a CPU-bound phase (spins) and
 * an interactive phase (sleep interval, short burst, repeat). Measures
 * wakeup-to-run latency in the interactive phases, and separately the
 * "recovery" latency: the first RECOV wakeups right after a CPU phase, i.e.
 * how fast the scheduler notices the task became interactive again.
 *
 * Usage: phase_change [-c cycles] [-C cpu_ms] [-I inter_ms] [-i interval_us]
 *                     [-w work_us] [-r recov_wakeups] [-o out.csv]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>

struct rec {
	int cycle;
	int idx;		/* wakeup index within the interactive phase */
	long long lat;
};

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;

	return (x > y) - (x < y);
}

static void pct(long long *v, int n, double *p50, double *p99, double *mx)
{
	qsort(v, n, sizeof(*v), cmp);
	*p50 = n ? v[n / 2] / 1e3 : 0;
	*p99 = n ? v[n * 99 / 100] / 1e3 : 0;
	*mx = n ? v[n - 1] / 1e3 : 0;
}

int main(int argc, char **argv)
{
	int cycles = 10, recov = 10, opt, c, n = 0, nr = 0, i;
	long long cpu_ms = 2000, inter_ms = 2000, interval = 10000, work = 200;
	const char *out = NULL;

	while ((opt = getopt(argc, argv, "c:C:I:i:w:r:o:")) != -1) {
		switch (opt) {
		case 'c': cycles = atoi(optarg); break;
		case 'C': cpu_ms = atoll(optarg); break;
		case 'I': inter_ms = atoll(optarg); break;
		case 'i': interval = atoll(optarg); break;
		case 'w': work = atoll(optarg); break;
		case 'r': recov = atoi(optarg); break;
		case 'o': out = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-c cycles] [-C cpu_ms] [-I inter_ms] [-i interval_us] [-w work_us] [-r recov] [-o csv]\n", argv[0]);
			return 1;
		}
	}

	prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);

	int cap = cycles * (int)(inter_ms * 1000 / interval + 2);
	struct rec *r = calloc(cap, sizeof(*r));
	long long *all = calloc(cap, sizeof(long long));
	long long *rv = calloc(cap, sizeof(long long));

	if (!r || !all || !rv)
		return 1;

	for (c = 0; c < cycles; c++) {
		/* CPU-bound phase */
		long long end = now_ns() + cpu_ms * 1000000LL;

		while (now_ns() < end)
			;

		/* interactive phase */
		long long phase_end = now_ns() + inter_ms * 1000000LL;
		long long target = now_ns();
		int idx = 0;

		for (;;) {
			struct timespec ts;
			long long woke, wend;

			target += interval * 1000;
			if (target >= phase_end || n >= cap)
				break;
			ts.tv_sec = target / 1000000000LL;
			ts.tv_nsec = target % 1000000000LL;
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
			woke = now_ns();

			r[n].cycle = c;
			r[n].idx = idx;
			r[n].lat = woke - target;
			n++;

			wend = woke + work * 1000;
			while (now_ns() < wend)
				;
			if (now_ns() > target + interval * 1000)
				target = now_ns();
			idx++;
		}
	}

	if (out) {
		FILE *f = fopen(out, "w");

		if (f) {
			fprintf(f, "cycle,idx,latency_ns\n");
			for (i = 0; i < n; i++)
				fprintf(f, "%d,%d,%lld\n", r[i].cycle, r[i].idx, r[i].lat);
			fclose(f);
		}
	}

	for (i = 0; i < n; i++) {
		all[i] = r[i].lat;
		if (r[i].idx < recov)
			rv[nr++] = r[i].lat;
	}

	double p50, p99, mx, rp50, rp99, rmx;

	pct(all, n, &p50, &p99, &mx);
	pct(rv, nr, &rp50, &rp99, &rmx);
	printf("phase latency (us): p50=%.1f p99=%.1f max=%.1f rec_p50=%.1f rec_p99=%.1f rec_max=%.1f (n=%d, recov=%d)\n",
	       p50, p99, mx, rp50, rp99, rmx, n, nr);
	return 0;
}
