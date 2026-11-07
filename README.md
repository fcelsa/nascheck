# nascheck

Disk health monitor for FreeBSD and Linux, in the terminal.
It reads SMART data of SATA (HDD and SSD) and NVMe disks through
`smartctl`, and the state of ZFS pools through `zpool`.

> **Status: early development (phase 4).** A daemon collects SMART data,
> temperatures and the state of ZFS pools into a local database; `status`
> and `check` report on it without root. The ncurses interface comes next.

## Requirements

- FreeBSD 14 or later, or Linux
- smartmontools 7.0 or later (JSON output)
- SQLite 3.35 or later
- for ZFS: OpenZFS 2.3 or later (`zpool status -j`), as in FreeBSD 15
- a C11 compiler and `make` (BSD make or GNU make)

```sh
pkg install smartmontools sqlite3                 # FreeBSD
apt install smartmontools libsqlite3-dev         # Debian, Ubuntu
```

## Build

```sh
make
make test        # runs against recorded smartctl output, no disks needed
make install     # program, sample configuration, rc.d script or
                 # systemd unit; PREFIX, SYSCONFDIR, DESTDIR honored
```

`make sanitize` rebuilds with AddressSanitizer and UBSan and runs the tests.

## The daemon

Only the daemon runs as root. It reads every disk once a day, samples the
temperatures every 5 minutes, and keeps both in a SQLite database. Then
`status` and `check` read the database as a normal user.

```sh
# FreeBSD
sysrc nascheck_enable=YES
service nascheck start

# Linux with systemd
systemctl enable --now nascheck
```

To let users read the data, choose a group in `nascheck.conf` (`group
operator`, say) and restart the daemon: the database and the socket become
readable by that group. Changed rules are applied with
`service nascheck reload` (SIGHUP). Messages go to syslog.

Disks in standby are not woken up: the daemon tries again every hour, and
wakes a disk only when its report is older than a week (`wake_after`).

## ZFS

`status` shows the pools as `zpool status` does, with the physical disk
under each vdev and that disk's health:

```console
POOL / VDEV                    STATE     READ WRITE CKSUM DISK   HEALTH NOTES
openasrz                       ONLINE                            WARN   3.0T, 34% used, resilver 2026-10-25
  raidz2-0                     ONLINE       0     0     0
    gpt/zfs-90eae6ca820c426b   ONLINE       0     0     0 ada5   WARN
    gpt/zfs-5f966b29ab9b3ddc   ONLINE       0     0     0 ada2   WARN
```

On FreeBSD the disk comes from the GEOM tree (`kern.geom.conftxt`), so
labels (`gpt/`, `gptid/`, `diskid/`) and partitions resolve to their disk;
on Linux from the device path (`/dev/disk/by-id/...-part1` -> `sdb`).
NVMe vdevs on FreeBSD (`nda`) are not mapped to their controller yet.

The daemon reads the pools at every temperature sample: `zpool status`
asks the kernel, not the disks, so a vdev that drops out is reported
within minutes.

## Usage

`status` and `check` read the daemon's database when it exists. Without
it, or with `-l`, they read the disks directly, which requires root
(`su`, or `sudo`/`doas` if installed).

```console
# nascheck status
DEVICE   HEALTH TYPE MODEL                    SERIAL                 SIZE  TEMP   HOURS SMART     TEST NOTES
ada0     WARN   HDD  TOSHIBA DT01ACA050       MASKED                 500G   32C   66998 PASSED  51956h realloc 0, pending 0, uncorr 0
ada1     WARN   HDD  TOSHIBA DT01ACA050       MASKED                 500G   30C   66980 PASSED  51835h realloc 0, pending 0, uncorr 0
ada2     WARN   HDD  WDC WD5000AADS-00S9B0    MASKED                 500G   29C   61431 PASSED  15088h realloc 0, pending 28, uncorr 10
...
# nascheck check
WARNING: 12 disks, 0 critical, 12 warning
ada0     WARNING  last self-test 51956 h ago (limit 720)
ada1     WARNING  last self-test 51835 h ago (limit 720)
ada2     WARNING  Current_Pending_Sector 28 (limit 1)
ada2     WARNING  Offline_Uncorrectable 10 (limit 1)
ada2     WARNING  last self-test 15088 h ago (limit 720)
...
```

| Command | Description |
| --- | --- |
| `nascheck scan` | list the devices reported by `smartctl --scan` |
| `nascheck status [device ...]` | table of all disks, or of the given ones |
| `nascheck check [device ...]` | summary line, then one line per problem |
| `nascheck daemon` | collect in the foreground (rc.d and systemd run it) |
| `nascheck refresh` | ask the daemon to read all disks now |

`TEST` is the number of power-on hours since the last SMART self-test.
Disks in standby are not woken up (`smartctl -n standby`). Devices that are
neither ATA nor NVMe, such as USB sticks, are left out of the scan.

Options go before the command: `-c file` reads another configuration,
`-l` reads the disks instead of the database, `-q` makes `check` print the
summary line only.

### Exit codes of `check`

Compatible with Nagios, Icinga and Zabbix plugins, and handy in cron:

| Code | Meaning |
| --- | --- |
| 0 | all disks OK |
| 1 | at least one warning |
| 2 | at least one critical problem |
| 3 | unknown: smartctl missing, bad configuration, unreadable disk or pool, daemon not updating the database |

### Rules

| Disks | Problem | Level |
| --- | --- | --- |
| all | SMART overall health test failed | critical |
| all | last self-test failed | critical |
| all | temperature at or above the limits | warning, critical |
| all | no self-test, or the last one older than `selftest_max_age` | warning |
| all | report in the database older than `smart_interval` + `wake_after` | warning |
| ATA | attribute failing now, by the disk's own threshold | critical if pre-failure, else warning |
| ATA | raw value of attributes 5, 187, 197, 198 above zero | warning |
| NVMe | critical warning bit set | critical |
| NVMe | wear (`percentage_used`) at 80% / 100% | warning, critical |
| NVMe | media errors above zero | warning |
| ZFS | pool not ONLINE, a vdev not ONLINE (spares: not AVAIL) | critical |
| ZFS | permanent errors in files | critical |
| ZFS | read, write or checksum errors on a vdev | warning |
| ZFS | allocated space at 80% / 90% | warning, critical |
| ZFS | no completed scrub, or the last one older than 35 days | warning |

Thresholds live in `/usr/local/etc/nascheck.conf`; see
[nascheck.conf.sample](nascheck.conf.sample), installed next to it, for
the keywords and the defaults used when the file does not exist.

## Roadmap

1. Disk discovery, `smartctl` JSON parsing, status table — **done**
2. Health rules from a config file, `nascheck check` with exit codes — **done**
3. Daemon: daily SMART report, temperature samples, SQLite, rc.d — **done**
4. ZFS pools: state, errors, scrub, vdev to disk mapping — **done**
5. ncurses interface: devices, details, history
6. Temperature graphs
7. Packaging: FreeBSD port, Linux static build

## Tests

`tests/fake-smartctl` replays the JSON reports in `tests/fixtures`, so the
whole program runs without disks. To record reports from a real machine
(serial numbers are masked):

```sh
sh scripts/capture-fixtures.sh   # as root
```

## License

MIT, see [LICENSE](LICENSE). Vendored libraries are listed in
[vendor/README.md](vendor/README.md).
