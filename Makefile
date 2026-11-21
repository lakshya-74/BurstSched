# BurstSched build. Reuses the kernel's tools/sched_ext build system so libbpf,
# bpftool, vmlinux.h and scx headers exactly match the running kernel.
KDIR ?= $(HOME)/burstsched-work/linux
SCX  := $(KDIR)/tools/sched_ext
SRCS := burstsched.bpf.c burstsched.c burstsched.h

.PHONY: all sched utils clean

all: sched utils

sched:
	@for f in $(SRCS); do ln -sf $(CURDIR)/src/$$f $(SCX)/$$f; done
	@rm -f $(SCX)/build/obj/sched_ext/burstsched* $(SCX)/build/include/burstsched* $(SCX)/build/bin/burstsched
	$(MAKE) -C $(SCX) c-sched-targets=burstsched burstsched
	@mkdir -p build results
	cp $(SCX)/build/bin/burstsched build/

utils: build/setext build/wakeup_lat

build/setext: utils/setext.c
	@mkdir -p build
	$(CC) -O2 -Wall -o $@ $<

build/wakeup_lat: workloads/wakeup_lat.c
	@mkdir -p build
	$(CC) -O2 -Wall -o $@ $<

clean:
	rm -rf build
	rm -f $(SCX)/build/obj/sched_ext/burstsched* $(SCX)/build/include/burstsched* $(SCX)/build/bin/burstsched
