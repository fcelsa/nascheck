# nascheck

Disk health monitor for FreeBSD and Linux, in the terminal.
It reads SMART data of SATA (HDD and SSD) and NVMe disks through
`smartctl`, and the state of ZFS pools through `zpool`.

> **Status: early development (phase 1).** Today nascheck lists the disks
> and prints a health table. The daemon, the history, ZFS and the
> ncurses interface come next; see the roadmap below.

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
make install     # into /usr/local/sbin, PREFIX and DESTDIR are honored
```

`make sanitize` rebuilds with AddressSanitizer and UBSan and runs the tests.

## Usage

Reading SMART data requires root (`su`, or `sudo`/`doas` if installed).

```console
# nascheck status
DEVICE   TYPE MODEL                    SERIAL                 SIZE  TEMP   HOURS SMART   TEST NOTES
ada0     HDD  WDC WD40EFRX-68N32N0     WD-WCC7K1ABCDEF        4.0T   41C   31204 PASSED  ok   realloc 8, pending 0, uncorr 0
ada1     SSD  Samsung SSD 870 EVO 1TB  S6PUNX0T123456A        1.0T   33C    9120 PASSED  -    realloc 0
ada2     -    -                        -                         -     -       - standby
nvme0    NVMe Samsung SSD 980 PRO 1TB  S5GXNX0T654321B        1.0T   47C    8123 PASSED  ok   used 3%, spare 100%, media err 0
```

| Command | Description |
| --- | --- |
| `nascheck scan` | list the devices reported by `smartctl --scan` |
| `nascheck status [device ...]` | health of all disks, or of the given ones |

Disks in standby are not woken up (`smartctl -n standby`). Devices that are
neither ATA nor NVMe, such as USB sticks, are left out of the scan.

## Roadmap

1. Disk discovery, `smartctl` JSON parsing, status table — **done**
2. Health rules from a config file, `nascheck check` with exit codes 0/1/2
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
