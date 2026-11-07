#!/bin/sh
# Run nascheck against the fixtures and compare with the expected output.
# usage: tests/run.sh path/to/nascheck

# Paths are relative to the top directory, as they appear in messages.
prog=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=$(cd "$(dirname "$0")" && pwd)
cd "$dir/.." || exit 1
NASCHECK_SMARTCTL=$dir/fake-smartctl
export NASCHECK_SMARTCTL
fail=0

# check NAME ARGS...: run nascheck ARGS and diff with expected/NAME.
# An empty configuration (/dev/null) means the built-in defaults, so an
# installed nascheck.conf cannot change the results.
check() {
	name=$1
	shift
	"$prog" "$@" >"$dir/$name.out" 2>&1
	echo "exit $?" >>"$dir/$name.out"
	if diff -u "$dir/expected/$name" "$dir/$name.out"; then
		echo "ok   $name"
	else
		echo "FAIL $name"
		fail=1
	fi
}

check scan scan
check status -c /dev/null status
check status-named -c /dev/null status /dev/ada0 /dev/ada3 /dev/da0 \
    /dev/missing
check check -c /dev/null check
check check-quiet -q -c /dev/null check
check check-critical -c /dev/null check /dev/ada4 /dev/nvme1 /dev/ada3 \
    /dev/ada2
check config-bad -c tests/conf/bad.conf check
check config-missing -c tests/conf/missing.conf status
check usage frobnicate

# Reports recorded on a real FreeBSD 15.1 NAS (12 SATA disks, ZFS).
NASCHECK_FIXTURES=$dir/fixtures/openas
export NASCHECK_FIXTURES
check openas-status -c /dev/null status
check openas-check -c /dev/null check
check openas-custom -c tests/conf/custom.conf check
# The sample configuration must match the built-in defaults.
cp "$dir/expected/openas-check" "$dir/expected/openas-sample"
check openas-sample -c nascheck.conf.sample check
rm -f "$dir/expected/openas-sample"

[ "$fail" -eq 0 ] && rm -f "$dir"/*.out
exit "$fail"
