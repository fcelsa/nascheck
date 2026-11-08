#!/bin/sh
# Save the raw output of smartctl and zpool from this machine, to be used
# as test fixtures. Serial numbers and WWNs are masked.
# Run it as root.
# usage: sh scripts/capture-fixtures.sh [outdir]

out=${1:-fixtures-$(hostname -s)}
mkdir -p "$out" || exit 1

mask() {
	sed -e 's/\("serial_number": "\)[^"]*"/\1MASKED"/' \
	    -e 's/\("id": \)[0-9]\{10,\}/\10/'
}

uname -a >"$out/uname.txt"
smartctl --version >"$out/smartctl-version.txt"
smartctl --scan -j >"$out/scan.json"
for dev in $(smartctl --scan | awk '{ print $1 }'); do
	smartctl -a -j -n standby "$dev" | mask >"$out/$(basename "$dev").json"
done

if command -v zpool >/dev/null 2>&1; then
	zpool version >"$out/zpool-version.txt"
	zpool status -j -p >"$out/zpool-status.json"
	zpool status -j --json-int -p >"$out/zpool-status-int.json"
	zpool status -P -p >"$out/zpool-status.txt"
	zpool list -H -p >"$out/zpool-list.txt"
	zfs list -H -p -o name,used,avail,refer,mountpoint \
	    -t filesystem,volume >"$out/zfs-list.txt"
fi
# Installed packages: name, version, size, install time, comment.
if command -v pkg >/dev/null 2>&1; then
	pkg query '%n\t%v\t%sb\t%t\t%a\t%#r\t%R\t%o\t%c' >"$out/pkg-query.txt"
	pkg -vv >"$out/pkg-vv.txt"
fi
if [ "$(uname -s)" = FreeBSD ]; then
	sysctl -n kern.geom.conftxt >"$out/geom-conftxt.txt"
	glabel status >"$out/glabel.txt"
	gpart show -p >"$out/gpart.txt"
fi
echo "saved in $out"
