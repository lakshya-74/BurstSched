# BurstSched build. Reuses the kernel's tools/sched_ext build system so libbpf,
# bpftool, vmlinux.h and scx headers exactly match the running kernel.
KDIR ?= $(HOME)/burstsched-work/linux
SCX  := $(KDIR)/tools/sched_ext
SRCS := burstsched.bpf.c burstsched.c burstsched.h waitmon.bpf.c waitmon.c waitmon.h
BPF_TARGETS := burstsched waitmon

.PHONY: all sched utils baselines clean

all: sched utils baselines

# scx_simple: the bundled minimal sched_ext scheduler, used as a baseline
baselines: build/scx_simple

build/scx_simple: | sched
	$(MAKE) -C $(SCX) scx_simple
	@mkdir -p build
	cp $(SCX)/build/bin/scx_simple build/

# burstsched (scheduler) and waitmon (wait-time tracer) are BPF programs built
# by the kernel's tools/sched_ext Makefile; always rebuilt from scratch.
sched:
	@for f in $(SRCS); do ln -sf $(CURDIR)/src/$$f $(SCX)/$$f; done
	@for t in $(BPF_TARGETS); do rm -f $(SCX)/build/obj/sched_ext/$$t* $(SCX)/build/include/$$t* $(SCX)/build/bin/$$t; done
	$(MAKE) -C $(SCX) c-sched-targets="$(BPF_TARGETS)" $(BPF_TARGETS)
	@mkdir -p build results
	for t in $(BPF_TARGETS); do cp $(SCX)/build/bin/$$t build/; done

utils: build/setext build/wakeup_lat build/phase_change build/rr_bench

build/phase_change: workloads/phase_change.c
	@mkdir -p build
	$(CC) -O2 -Wall -o $@ $<

build/setext: utils/setext.c
	@mkdir -p build
	$(CC) -O2 -Wall -o $@ $<

build/wakeup_lat: workloads/wakeup_lat.c
	@mkdir -p build
	$(CC) -O2 -Wall -o $@ $<

build/rr_bench: workloads/rr_bench.c
	@mkdir -p build
	$(CC) -O2 -Wall -pthread -o $@ $<

clean:
	rm -rf build
	for t in $(BPF_TARGETS); do rm -f $(SCX)/build/obj/sched_ext/$$t* $(SCX)/build/include/$$t* $(SCX)/build/bin/$$t; done
