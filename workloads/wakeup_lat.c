/*
 * wakeup_lat: interactive-task simulator. Sleeps until an absolute deadline,
 * measures how late it actually ran (wakeup-to-run latency), does a short
 * CPU burst, repeats. Prints p50/p90/p99/max at the end.
 *
 * Usage: wakeup_lat [-n iters] [-i interval_us] [-w work_us] [-o out.csv]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>

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

int main(int argc, char **argv)
{
	int n = 2000, opt, i;
	long long interval = 10000, work = 200;	/* microseconds */
	const char *out = NULL;
	long long *lat;
	FILE *f = NULL;

	while ((opt = getopt(argc, argv, "n:i:w:o:")) != -1) {
		switch (opt) {
		case 'n': n = atoi(optarg); break;
		case 'i': interval = atoll(optarg); break;
		case 'w': work = atoll(optarg); break;
		case 'o': out = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-n iters] [-i interval_us] [-w work_us] [-o out.csv]\n", argv[0]);
			return 1;
		}
	}

	/* default timer slack (50us) would hide scheduling latency */
	prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);

	lat = calloc(n, sizeof(*lat));
	if (!lat)
		return 1;

	long long target = now_ns();

	for (i = 0; i < n; i++) {
		struct timespec ts;
		long long woke, end;

		target += interval * 1000;
		ts.tv_sec = target / 1000000000LL;
		ts.tv_nsec = target % 1000000000LL;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
		woke = now_ns();
		lat[i] = woke - target;

		end = woke + work * 1000;	/* short CPU burst */
		while (now_ns() < end)
			;
		if (now_ns() > target + interval * 1000)
			target = now_ns();	/* fell behind; resync */
	}

	if (out && (f = fopen(out, "w"))) {
		fprintf(f, "iter,latency_ns\n");
		for (i = 0; i < n; i++)
			fprintf(f, "%d,%lld\n", i, lat[i]);
		fclose(f);
	}

	qsort(lat, n, sizeof(*lat), cmp);
	printf("wakeup latency (us): p50=%.1f p90=%.1f p99=%.1f max=%.1f  (n=%d)\n",
	       lat[n / 2] / 1e3, lat[n * 9 / 10] / 1e3, lat[n * 99 / 100] / 1e3,
	       lat[n - 1] / 1e3, n);
	free(lat);
	return 0;
}
