"""Give the daemon's test database a past, for the trends.

usage: python3 tests/history.py DATABASE

The daemon has just stored one report per disk and one row per pool. Add
older copies of them, with the counters changed:
  ada0   6.5 days ago: 3 fewer reallocated sectors, 4 fewer CRC errors
  ada1   29 days ago: 1% less wear, 150 GB less written
  nvme0  29 days ago: 2% less wear, 145 GB less written
  tank   daily rows over 28 days, growing 0.3% a day: full in 50
  backup daily rows over 28 days, steady
"""
import sqlite3
import sys
import time

DAY = 86400
db = sqlite3.connect(sys.argv[1])
now = int(time.time())


def past(path, ago, changes):
    disk = db.execute("SELECT id FROM disk WHERE path = ?", (path,)).fetchone()[0]
    snap, level, temp, hours = db.execute(
        "SELECT id, level, temp, hours FROM snapshot WHERE disk = ?"
        " ORDER BY time DESC LIMIT 1", (disk,)).fetchone()
    new = db.execute(
        "INSERT INTO snapshot (disk, time, level, temp, hours)"
        " VALUES (?, ?, ?, ?, ?)",
        (disk, now - ago, level, temp, hours - ago // 3600)).lastrowid
    for ident, raw, value in db.execute(
            "SELECT id, raw, value FROM attribute WHERE snapshot = ?",
            (snap,)).fetchall():
        if ident in changes:
            raw, value = changes[ident](raw, value)
        db.execute("INSERT INTO attribute VALUES (?, ?, ?, ?)",
                   (new, ident, raw, value))


def pool(name, start, perday):
    size, alloc = db.execute(
        "SELECT size, alloc FROM pool WHERE name = ? ORDER BY time DESC"
        " LIMIT 1", (name,)).fetchone()
    for k in range(28, 0, -1):
        db.execute("INSERT INTO pool VALUES (?, ?, 'ONLINE', ?, ?, 0, 0)",
                   (name, now - k * DAY,
                    int(size * (start + perday * (28 - k)) / 100), size))


past("/dev/ada0", 6 * DAY + DAY // 2,
     {5: lambda r, v: (r - 3, v), 199: lambda r, v: (r - 4, v)})
past("/dev/ada1", 29 * DAY,
     {177: lambda r, v: (r, v + 1),
      241: lambda r, v: (r - 150 * 10**9 // 512, v)})
past("/dev/nvme0", 29 * DAY,
     {1001: lambda r, v: (r - 2, v),
      1006: lambda r, v: (r - 145 * 10**9 // 512000, v)})
pool("tank", 76.6, 0.3)
pool("backup", 16.7, 0.0)
db.commit()
