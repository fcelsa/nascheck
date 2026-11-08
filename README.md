# nascheck

Disk health monitor for FreeBSD and Linux, in the terminal.
It reads SMART data of SATA (HDD and SSD) and NVMe disks through
`smartctl`, and the state of ZFS pools through `zpool`.

> **Status: early development (phase 5).** A daemon collects SMART data,
> temperatures and the state of ZFS pools into a local database; a
> terminal interface, `status` and `check` report on it without root.

## Requirements

- FreeBSD 14 or later, or Linux
- smartmontools 7.0 or later (JSON output)
- SQLite 3.35 or later
- ncurses with wide characters (in the FreeBSD base system)
- for ZFS: OpenZFS 2.3 or later (`zpool status -j`), as in FreeBSD 15
- a C11 compiler and `make` (BSD make or GNU make)

```sh
pkg install smartmontools sqlite3                         # FreeBSD
apt install smartmontools libsqlite3-dev libncurses-dev   # Debian, Ubuntu
```

## Build

```sh
make
make test        # runs against recorded smartctl output, no disks needed
make install     # program, sample configuration, rc.d script or
                 # systemd unit; PREFIX, SYSCONFDIR, DESTDIR honored
```

`make sanitize` rebuilds with AddressSanitizer and UBSan and runs the tests.

## The interface

`nascheck` without a command opens a terminal interface in the style of
btop. It reads the daemon's database as a normal user, so it needs no
root; 120x40 is the reference size, 80x24 the minimum.

- **Disks** (page 1): disks and pools on the left; on the right the
  details of the selection (findings, SMART attributes and how they
  changed over the last 30 days; for a pool its usable and raw size, what
  its layout survives, the vdev tree with the `/dev` node and health of
  each disk, and its datasets); below, a graph of the temperature of the
  selected disk, or of the space used by the selected pool.
- **System** (page 2): the machine at a glance, in the style of fastfetch:
  network address, terminal, OS, hardware, memory, swap, disks and pools
  with bars of the space used, uptime.
- **Packages** (page 3): the packages installed (pkg, dpkg or rpm) with a
  search that is always active: every word typed must match the name or
  the description; the best matches come first (exact name, prefix,
  name, description, then letters in order: `zfst` finds `zfs-stats`) and
  the matching text is highlighted. Each package is marked as installed
  by hand (●), as a dependency (○) or part of the base system (◆), with
  its repository and how many packages need it; below, the enabled
  repositories and how the base system is updated. `S` sorts by name,
  size or install date.

| Key | Action |
| --- | --- |
| `1`-`3`, `F1`-`F3`, Tab, Shift-Tab | page |
| arrows, `j` `k`, `g` `G` | select |
| PgUp, PgDn | scroll the details (page 1) or the list (page 3) |
| `t`, `T` | graph period: 1h, 24h, 7d, 30d |
| `r`, `F5` | ask the daemon to read all disks now |
| Esc, ^U | clear the search (page 3) |
| `S` | sort by name, size or date (page 3) |
| `q`, `F10` | quit |

On the Packages page letters go to the search field, so there the page
keys are F1-F3 and Tab, refresh is F5 and quit is F10; the search ignores
case, so a capital `S` sorts.

| Source | By hand / dependency | Base system | Repositories | Install date |
| --- | --- | --- | --- | --- |
| pkg (FreeBSD) | `%a` | `FreeBSD-*` packages (pkgbase), else freebsd-update | `pkg -vv`, and each package's | yes |
| dpkg (Debian, Ubuntu, Proxmox) | apt `extended_states` | priority required, important or essential | `sources.list`, `.list`, `.sources` | dpkg's file lists |
| pacman (Arch, CachyOS, Manjaro) | `%REASON%` | `base` and what it depends on | `pacman.conf` | yes |
| apk (Alpine) | `/etc/apk/world` | `alpine-base` and what it depends on | `/etc/apk/repositories` | — |
| rpm + dnf (Fedora, RHEL, Rocky, Alma) | dnf's reason | installed by group (the installer) | `dnf repolist`, and each package's | yes |
| rpm + zypper (openSUSE) | `AutoInstalled` | — | `zypp/repos.d` | yes |
| flatpak | applications / runtimes | — | each package's remote | deployment date |
| snap | applications / bases | snapd, core, kernel, gadget | each package's channel | snap file date |

The first package manager found is read, then flatpak and snap. Other
installers (pip, npm, Homebrew, Nix, AppImage, `make install`) keep no
common record and are not listed.

Graphs use Braille characters in a UTF-8 terminal and plain characters
elsewhere (the FreeBSD console without X); 256 colors when the terminal
has them, 8 otherwise. Idle, the interface wakes once a second for the
clock and every 5 seconds looks for new data from the daemon.

Pages are modules (`src/page_*.c`): a new one is a file with its draw and
key functions, listed in `src/tui.c`.

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
| `nascheck` | the terminal interface |
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
5. ncurses interface with pages: disks and pools, system, packages — **done**
6. Temperature graphs — **done**; next: a page with all disks and sensors
7. Packaging: FreeBSD port, Linux static build

## Tests

`tests/fake-smartctl`, `fake-zpool`, `fake-zfs` and `fake-sysctl` replay
the reports in `tests/fixtures`, so the whole program runs without disks or
pools. Packages are read from a fake root (`NASCHECK_ROOT`, one per system
in `tests/roots`: database files, and stand-in commands in `bin/`). The
interface is tested with `NASCHECK_TUI_SNAPSHOT=keys`: it takes the keys,
prints one screen as text and exits. To record reports from a real machine
(serial numbers are masked):

```sh
sh scripts/capture-fixtures.sh   # as root
```

## License

MIT, see [LICENSE](LICENSE). Vendored libraries are listed in
[vendor/README.md](vendor/README.md).
