#!/bin/bash
# One-shot setup of a lab PC for BurstSched.
# Run as the normal user from the project folder:  ./scripts/setup_lab.sh
# (asks for the sudo password once, for apt and the load test)
set -u
cd "$(dirname "$0")/.."
PROJ=$(pwd)
WORK=$(dirname "$PROJ")
FAIL=0
ok()  { echo "  [OK]   $*"; }
bad() { echo "  [FAIL] $*"; FAIL=1; }

echo "== 1. Kernel checks"
KREL=$(uname -r)
echo "  kernel $KREL, $(nproc) CPUs, $(. /etc/os-release; echo "$PRETTY_NAME")"
grep -q '^CONFIG_SCHED_CLASS_EXT=y' "/boot/config-$KREL" 2>/dev/null \
	&& ok "CONFIG_SCHED_CLASS_EXT=y" || bad "sched_ext not enabled in this kernel"
[ -d /sys/kernel/sched_ext ] && ok "/sys/kernel/sched_ext present" || bad "/sys/kernel/sched_ext missing"
[ -e /sys/kernel/btf/vmlinux ] && ok "BTF present" || bad "/sys/kernel/btf/vmlinux missing"
id -nG | grep -qw sudo && ok "user is in sudo group" || bad "user not in sudo group"
if [ $FAIL != 0 ]; then echo "This PC can't run sched_ext schedulers. Stop here."; exit 1; fi

echo "== 2. Internet"
if curl -s -m 10 -o /dev/null https://github.com; then
	ok "internet works"
else
	bad "no internet. Run ./scripts/fwlogin.sh, then run this script again"; exit 1
fi

echo "== 3. Packages (needs sudo)"
sudo -v || { bad "sudo failed"; exit 1; }
sudo apt-get update -qq > /dev/null 2>&1
# apt may report errors from unrelated half-installed packages (e.g. NVIDIA);
# we check the tools we need afterwards instead of trusting the exit code.
sudo apt-get install -y clang llvm libelf-dev zlib1g-dev pkg-config libzstd-dev \
	make gcc git stress-ng python3-venv python3-pip > "$WORK/apt.log" 2>&1
for b in clang llc make gcc git stress-ng python3; do
	command -v "$b" > /dev/null && ok "$b" || bad "$b missing (see $WORK/apt.log)"
done
[ -e /usr/include/libelf.h ] && ok "libelf-dev" || bad "libelf-dev missing"
if [ $FAIL != 0 ]; then exit 1; fi

echo "== 4. Kernel source"
MAJMIN=$(echo "$KREL" | cut -d. -f1,2)
if [ -d "$WORK/linux/tools/sched_ext" ]; then
	ok "kernel source already at $WORK/linux"
else
	echo "  cloning v$MAJMIN (~280 MB)..."
	git clone -q --depth 1 --branch "v$MAJMIN" https://github.com/torvalds/linux.git "$WORK/linux" \
		&& ok "cloned v$MAJMIN" || { bad "clone failed"; exit 1; }
fi

echo "== 5. Build (first time takes a few minutes)"
if make KDIR="$WORK/linux" -j"$(nproc)" > "$WORK/build.log" 2>&1; then
	ok "build: $(ls build | tr '\n' ' ')"
else
	bad "build failed. First errors:"
	grep -v -E "modification time|Clock skew" "$WORK/build.log" | grep -E -A3 "error" | head -30
	exit 1
fi

echo "== 6. Load test"
sudo -v
sudo timeout 5 ./build/burstsched -p -q > "$WORK/smoke.log" 2>&1 &
sleep 3
if [ "$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)" = "burstsched" ]; then
	ok "BurstSched loads and runs"
else
	bad "scheduler did not load. Log:"; cat "$WORK/smoke.log"
fi
wait
sleep 1
echo "  sched_ext state now: $(cat /sys/kernel/sched_ext/state)"

[ $FAIL = 0 ] && echo "== ALL GOOD. Ready to run scripts/quick_compare.sh" || exit 1
