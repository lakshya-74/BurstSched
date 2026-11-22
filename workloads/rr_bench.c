/*
 * rr_bench: request/response service benchmark (a more realistic interactive
 * workload than wakeup_lat). A server process answers fixed-size requests over
 * loopback TCP, doing `work_us` of CPU work per request; `conns` client
 * threads send requests on a fixed (open-loop) schedule and measure the time
 * from the scheduled send time to the reply (so client wakeup delay counts too).
 *
 * Usage: rr_bench [-c conns] [-r req_per_s_per_conn] [-d seconds] [-w work_us] [-o out.csv]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MSG 64

static int conns = 4, rate = 200, dur = 30;
static long long work_us = 100;
static struct sockaddr_in addr;

struct client {
	pthread_t th;
	long long *lat;
	int n, cap;
};

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int rw_full(int fd, char *buf, int len, int wr)
{
	int done = 0;

	while (done < len) {
		ssize_t r = wr ? write(fd, buf + done, len - done)
			       : read(fd, buf + done, len - done);
		if (r <= 0)
			return -1;
		done += r;
	}
	return 0;
}

static void *serve(void *arg)
{
	int fd = (int)(long)arg;
	char buf[MSG];

	for (;;) {
		long long end;

		if (rw_full(fd, buf, MSG, 0))
			break;
		end = now_ns() + work_us * 1000;	/* request handling */
		while (now_ns() < end)
			;
		if (rw_full(fd, buf, MSG, 1))
			break;
	}
	close(fd);
	return NULL;
}

static void run_server(int lfd)
{
	for (;;) {
		int fd = accept(lfd, NULL, NULL), one = 1;
		pthread_t th;

		if (fd < 0)
			continue;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		pthread_create(&th, NULL, serve, (void *)(long)fd);
		pthread_detach(th);
	}
}

static void *client(void *arg)
{
	struct client *c = arg;
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	long long period = 1000000000LL / rate, target, end;
	char buf[MSG] = "req";

	prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
		perror("connect");
		return NULL;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	target = now_ns();
	end = target + dur * 1000000000LL;
	while (c->n < c->cap) {
		struct timespec ts;

		target += period;
		if (target >= end)
			break;
		ts.tv_sec = target / 1000000000LL;
		ts.tv_nsec = target % 1000000000LL;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
		if (rw_full(fd, buf, MSG, 1) || rw_full(fd, buf, MSG, 0))
			break;
		c->lat[c->n++] = now_ns() - target;
		if (now_ns() > target + period)
			target = now_ns();	/* fell behind; resync */
	}
	close(fd);
	return NULL;
}

static int cmp(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;

	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	const char *out = NULL;
	int opt, lfd, one = 1, i, total = 0, k = 0;
	socklen_t alen = sizeof(addr);
	struct client *cl;
	long long *all;
	pid_t srv;

	while ((opt = getopt(argc, argv, "c:r:d:w:o:")) != -1) {
		switch (opt) {
		case 'c': conns = atoi(optarg); break;
		case 'r': rate = atoi(optarg); break;
		case 'd': dur = atoi(optarg); break;
		case 'w': work_us = atoll(optarg); break;
		case 'o': out = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-c conns] [-r req/s per conn] [-d s] [-w work_us] [-o csv]\n", argv[0]);
			return 1;
		}
	}
	if (conns < 1 || rate < 1 || dur < 1)
		return 1;

	/* listen on an ephemeral loopback port before forking the server */
	lfd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) ||
	    listen(lfd, 64) || getsockname(lfd, (struct sockaddr *)&addr, &alen)) {
		perror("listen");
		return 1;
	}

	srv = fork();
	if (srv == 0) {
		prctl(PR_SET_NAME, "rr_server");
		run_server(lfd);
		_exit(0);
	}
	close(lfd);

	cl = calloc(conns, sizeof(*cl));
	for (i = 0; i < conns; i++) {
		cl[i].cap = rate * dur + 16;
		cl[i].lat = calloc(cl[i].cap, sizeof(long long));
		pthread_create(&cl[i].th, NULL, client, &cl[i]);
	}
	for (i = 0; i < conns; i++) {
		pthread_join(cl[i].th, NULL);
		total += cl[i].n;
	}
	kill(srv, SIGTERM);
	waitpid(srv, NULL, 0);

	all = calloc(total ? total : 1, sizeof(long long));
	for (i = 0; i < conns; i++) {
		memcpy(all + k, cl[i].lat, cl[i].n * sizeof(long long));
		k += cl[i].n;
	}

	if (out) {
		FILE *f = fopen(out, "w");

		if (f) {
			fprintf(f, "req,latency_ns\n");
			for (i = 0; i < total; i++)
				fprintf(f, "%d,%lld\n", i, all[i]);
			fclose(f);
		}
	}

	if (!total) {
		printf("rr latency (us): no requests completed\n");
		return 1;
	}
	qsort(all, total, sizeof(long long), cmp);
	printf("rr latency (us): p50=%.1f p90=%.1f p99=%.1f max=%.1f  (n=%d, %.0f req/s)\n",
	       all[total / 2] / 1e3, all[total * 9 / 10] / 1e3,
	       all[total * 99 / 100] / 1e3, all[total - 1] / 1e3, total,
	       (double)total / dur);
	return 0;
}
