# Changelog

## 1.0.0 — 2026-11-08

The first release.

- Disks: SATA (HDD, SSD) and NVMe through `smartctl` JSON, judged against
  rules from `/usr/local/etc/nascheck.conf`: SMART status, self-tests,
  temperatures, reallocated, pending and uncorrectable sectors, NVMe wear,
  spare and media errors.
- ZFS: pool and vdev state, errors, scrub age, usable and raw size, what
  each layout survives, the `/dev` node behind each vdev, datasets.
- Trends from the daemon's history: counters that grew over a week, CRC
  errors that point to a cable or backplane, SSD and NVMe wear and pool
  growth projected, datasets whose snapshots stopped.
- `nascheck status` and `nascheck check` (Nagios-style exit codes) for
  scripts and monitoring.
- A daemon (rc.d on FreeBSD, systemd on Linux) that reads every disk once
  a day without waking sleeping ones and samples temperatures every five
  minutes into SQLite, readable by a group of normal users.
- A terminal interface in four pages: Disks (details, attribute changes,
  temperature and space graphs), System (fastfetch style, with the
  temperature sensors), Packages (pkg, dpkg, pacman, apk, rpm with dnf or
  zypper, flatpak and snap, with a live ranked search) and Health (every
  problem and note, the worst first).
- FreeBSD 15 with OpenZFS 2.3 or later; Linux: Debian, Ubuntu, Proxmox,
  Arch and its derivatives, Alpine, Fedora, RHEL and its rebuilds,
  openSUSE.
- Binaries: static, for FreeBSD amd64 and Linux x86_64 and aarch64.
