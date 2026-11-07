# nascheck

Disk health monitor for FreeBSD and Linux, in the terminal.
It reads SMART data of SATA (HDD and SSD) and NVMe disks through
`smartctl`, and the state of ZFS pools through `zpool`.

> **Status: early development (phase 2).** nascheck reads the disks, judges
> them against configurable rules and reports problems. The daemon, the
> history, ZFS and the ncurses interface come next; see the roadmap below.

## Requirements

- FreeBSD 14 or later, or Linux
- smartmontools 7.0 or later (JSON output)
- a C11 compiler and `make` (BSD make or GNU make)

On FreeBSD everything but smartmontools is in the base system:

```sh
pkg install smartmontools
```

## Build

```sh
make
make test        # runs against recorded smartctl output, no disks needed
make install     # into /usr/local/sbin and /usr/local/etc;
                 # PREFIX, SYSCONFDIR and DESTDIR are honored
```

`make sanitize` rebuilds with AddressSanitizer and UBSan and runs the tests.

## Usage

Reading SMART data requires root (`su`, or `sudo`/`doas` if installed).

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

`TEST` is the number of power-on hours since the last SMART self-test.
Disks in standby are not woken up (`smartctl -n standby`). Devices that are
neither ATA nor NVMe, such as USB sticks, are left out of the scan.

Options go before the command: `-c file` reads another configuration,
`-q` makes `check` print the summary line only.

### Exit codes of `check`

Compatible with Nagios, Icinga and Zabbix plugins, and handy in cron:

| Code | Meaning |
| --- | --- |
| 0 | all disks OK |
| 1 | at least one warning |
| 2 | at least one critical problem |
| 3 | unknown: smartctl missing, bad configuration, unreadable disk |

### Rules

| Disks | Problem | Level |
| --- | --- | --- |
| all | SMART overall health test failed | critical |
| all | last self-test failed | critical |
| all | temperature at or above the limits | warning, critical |
| all | no self-test, or the last one older than `selftest_max_age` | warning |
| ATA | attribute failing now, by the disk's own threshold | critical if pre-failure, else warning |
| ATA | raw value of attributes 5, 187, 197, 198 above zero | warning |
| NVMe | critical warning bit set | critical |
| NVMe | wear (`percentage_used`) at 80% / 100% | warning, critical |
| NVMe | media errors above zero | warning |

Thresholds live in `/usr/local/etc/nascheck.conf`; see
[nascheck.conf.sample](nascheck.conf.sample), installed next to it, for
the keywords and the defaults used when the file does not exist.

## Roadmap

1. Disk discovery, `smartctl` JSON parsing, status table — **done**
2. Health rules from a config file, `nascheck check` with exit codes — **done**
3. Daemon: daily SMART snapshot and frequent temperature samples in SQLite
4. ZFS pools: state, errors, scrub, vdev to disk mapping
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
