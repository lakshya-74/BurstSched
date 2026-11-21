/*
 * setext: run a command under the SCHED_EXT policy.
 * Needed in partial mode (-p): only SCHED_EXT tasks are handled by BurstSched.
 * Children inherit the policy.   Usage: setext <cmd> [args...]
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

#ifndef SCHED_EXT
#define SCHED_EXT 7
#endif

int main(int argc, char **argv)
{
	struct sched_param sp = { .sched_priority = 0 };

	if (argc < 2) {
		fprintf(stderr, "usage: %s <cmd> [args...]\n", argv[0]);
		return 1;
	}
	if (sched_setscheduler(0, SCHED_EXT, &sp)) {
		perror("sched_setscheduler(SCHED_EXT)");
		return 1;
	}
	execvp(argv[1], argv + 1);
	perror("execvp");
	return 127;
}
