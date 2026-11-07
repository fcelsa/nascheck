#!/bin/sh
# Run nascheck against the fixtures and compare with the expected output.
# usage: tests/run.sh path/to/nascheck

prog=$1
dir=$(cd "$(dirname "$0")" && pwd)
NASCHECK_SMARTCTL=$dir/fake-smartctl
export NASCHECK_SMARTCTL
fail=0

# check NAME ARGS...: run nascheck ARGS and diff with expected/NAME.
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
check status status
check status-named status /dev/ada0 /dev/ada3 /dev/da0 /dev/missing

[ "$fail" -eq 0 ] && rm -f "$dir"/*.out
exit "$fail"
