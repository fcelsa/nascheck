#!/bin/sh
# Run nascheck against the fixtures and compare with the expected output.
# usage: tests/run.sh path/to/nascheck

# Paths are relative to the top directory, as they appear in messages.
prog=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=$(cd "$(dirname "$0")" && pwd)
cd "$dir/.." || exit 1
# Fakes for smartctl, zpool, zfs and sysctl replay the fixtures; packages
# come from a fake root (roots/*: files, and commands in bin/); dates are
# printed in UTC.
NASCHECK_SMARTCTL=$dir/fake-smartctl
NASCHECK_ZPOOL=$dir/fake-zpool
NASCHECK_ZFS=$dir/fake-zfs
NASCHECK_SYSCTL=$dir/fake-sysctl
NASCHECK_ROOT=$dir/roots/freebsd
TZ=UTC
export NASCHECK_SMARTCTL NASCHECK_ZPOOL NASCHECK_ZFS NASCHECK_SYSCTL \
    NASCHECK_ROOT TZ
fail=0

# check NAME ARGS...: run nascheck ARGS and diff with expected/NAME.
# tests/conf/base.conf keeps the built-in rules and points to a database
# that does not exist, so an installed nascheck cannot change the results.
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
check status -c tests/conf/base.conf status
check status-named -c tests/conf/base.conf status /dev/ada0 /dev/ada3 /dev/da0 \
    /dev/missing
check check -c tests/conf/base.conf check
check check-quiet -q -c tests/conf/base.conf check
check check-critical -c tests/conf/base.conf check /dev/ada4 /dev/nvme1 /dev/ada3 \
    /dev/ada2
check config-bad -c tests/conf/bad.conf check
check config-missing -c tests/conf/missing.conf status
check usage frobnicate

# The daemon, with its database and socket in a temporary directory.
tmp=$(mktemp -d)
printf 'database %s/db/nascheck.db\nsocket %s/nascheck.sock\n' \
    "$tmp" "$tmp" >"$tmp/conf"
"$prog" -c "$tmp/conf" daemon 2>/dev/null &
pid=$!
i=0
while [ ! -S "$tmp/nascheck.sock" ] && [ "$i" -lt 50 ]; do
	sleep 0.1
	i=$((i + 1))
done
check daemon-refresh -c "$tmp/conf" refresh
check daemon-status -c "$tmp/conf" status
check daemon-check -c "$tmp/conf" check
check daemon-live -l -c "$tmp/conf" check
kill "$pid"
wait "$pid"
if [ -e "$tmp/nascheck.sock" ]; then
	echo "FAIL daemon-stop: socket left behind"
	fail=1
else
	echo "ok   daemon-stop"
fi
rm -rf "$tmp"

# The terminal interface: one screen printed as text after some keys
# (NASCHECK_TUI_SNAPSHOT), reading the disks directly (-l).
TERM=xterm-256color
LC_ALL=C.UTF-8
LINES=24
COLUMNS=80
NASCHECK_TUI_SNAPSHOT=jjjj
export TERM LC_ALL LINES COLUMNS NASCHECK_TUI_SNAPSHOT
check tui-small -l -c tests/conf/base.conf
LC_ALL=C check tui-ascii -l -c tests/conf/base.conf
# The system page depends on the host: only its layout is checked.
NASCHECK_TUI_SNAPSHOT=2
if "$prog" -l -c tests/conf/base.conf | grep -q 'Uptime .* minute'; then
	echo "ok   tui-system"
else
	echo "FAIL tui-system"
	fail=1
fi
LINES=40
COLUMNS=120
# Packages from pkg: base system, by hand, dependencies; the S orders.
NASCHECK_TUI_SNAPSHOT='@3@d@d'
check tui-packages -l -c tests/conf/base.conf
NASCHECK_TUI_SNAPSHOT='@3S'
check tui-packages-size -l -c tests/conf/base.conf
NASCHECK_TUI_SNAPSHOT='@3zfSS@d'
check tui-packages-date -l -c tests/conf/base.conf

# The other package managers, newest first. Dates come from files here:
# set them, git does not keep them.
r=$dir/roots
touch -t 202606041200 "$r"/debian/var/lib/dpkg/info/*.list \
    "$r"/debian/var/lib/snapd/snaps/core22_2133.snap
touch -t 202610251200 "$r"/debian/var/lib/dpkg/info/smartmontools:amd64.list \
    "$r"/debian/var/lib/dpkg/info/zfsutils-linux:amd64.list
touch -t 202611011200 "$r"/debian/var/lib/snapd/snaps/firefox_7177.snap
NASCHECK_TUI_SNAPSHOT='@3SS'
for distro in debian arch alpine fedora opensuse; do
	NASCHECK_ROOT=$r/$distro
	check "tui-packages-$distro" -l -c tests/conf/base.conf
done
NASCHECK_ROOT=$r/freebsd

# The first reports recorded on a real FreeBSD 15.1 NAS (12 SATA disks,
# ZFS): self-tests long ago or never, pools never scrubbed, a resilver.
NASCHECK_FIXTURES=$dir/fixtures/openas-first
export NASCHECK_FIXTURES
check openas-first-status -c tests/conf/base.conf status
check openas-first-check -c tests/conf/base.conf check

# The same NAS 17 hours later: fresh self-tests and scrubs, one failed
# self-test (ada2), its packages.
NASCHECK_FIXTURES=$dir/fixtures/openas
check openas-status -c tests/conf/base.conf status
check openas-check -c tests/conf/base.conf check
check openas-custom -c tests/conf/custom.conf check
NASCHECK_TUI_SNAPSHOT=jj
check openas-tui-disk -l -c tests/conf/base.conf
NASCHECK_TUI_SNAPSHOT=Gkkt
check openas-tui-pool -l -c tests/conf/base.conf
NASCHECK_TUI_SNAPSHOT=G@n
check openas-tui-datasets -l -c tests/conf/base.conf
NASCHECK_TUI_SNAPSHOT='@3pyth@d@d'
check openas-tui-packages -l -c tests/conf/base.conf
NASCHECK_TUI_SNAPSHOT='@3qzzx'
check openas-tui-nomatch -l -c tests/conf/base.conf
unset NASCHECK_TUI_SNAPSHOT
# The sample configuration must match the built-in defaults.
cp "$dir/expected/openas-check" "$dir/expected/openas-sample"
check openas-sample -l -c nascheck.conf.sample check
rm -f "$dir/expected/openas-sample"

[ "$fail" -eq 0 ] && rm -f "$dir"/*.out
exit "$fail"
