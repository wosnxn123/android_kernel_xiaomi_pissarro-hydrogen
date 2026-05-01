#!/system/bin/sh
# HydrogenX v14 runtime sanity check.
set -u

fail=0
say() { echo "[HX14] $*"; }
check_file() {
	if [ -e "$1" ]; then
		say "OK: $1"
	else
		say "MISS: $1"
		fail=1
	fi
}

say "kernel: $(uname -a)"
check_file /sys/kernel/hydrogenx/version
check_file /sys/kernel/hydrogenx/status
check_file /sys/kernel/hydrogenx/builtins

for p in /sys/devices/system/cpu/cpufreq/policy*; do
	[ -d "$p" ] || continue
	if [ -f "$p/scaling_available_governors" ]; then
		say "$p governors: $(cat "$p/scaling_available_governors")"
		grep -q hydrogenx "$p/scaling_available_governors" || fail=1
	fi
	done

if [ -e /sys/block/zram0/comp_algorithm ]; then
	say "zram0 algorithms: $(cat /sys/block/zram0/comp_algorithm)"
fi

exit $fail
