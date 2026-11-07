/* zfs.h - ZFS pools through `zpool status -j`, and their physical disks. */
#ifndef NASCHECK_ZFS_H
#define NASCHECK_ZFS_H

#include <stddef.h>
#include <stdint.h>

#include "run.h"

#define ZFS_NAME	64

/* Where a vdev sits in the pool layout. */
enum vdev_section {
	SECTION_DATA,		/* data vdevs, including special and dedup */
	SECTION_LOGS,
	SECTION_CACHE,
	SECTION_SPARES
};

struct vdev {
	char			name[ZFS_NAME];	/* gpt/LABEL, mirror-0, ada9 */
	char			type[12];	/* disk, file, mirror, raidz */
	char			state[12];	/* ONLINE, DEGRADED, OFFLINE */
	char			path[128];	/* leaves: the device ZFS opens */
	char			disk[32];	/* physical disk node or "" */
	int64_t			read_errors;
	int64_t			write_errors;
	int64_t			checksum_errors;
	uint8_t			depth;		/* 0 for top-level vdevs */
	enum vdev_section	section;
};

struct pool {
	char		 name[ZFS_NAME];
	char		 state[12];
	int64_t		 data_errors;	/* permanent errors in files */
	int64_t		 alloc;		/* bytes */
	int64_t		 size;
	char		 scan_func[12];	/* SCRUB, RESILVER, "" if none */
	char		 scan_state[12];	/* SCANNING, FINISHED, CANCELED */
	int64_t		 scan_end;	/* unix time, 0 if unknown */
	int64_t		 scan_errors;
	struct vdev	*vdevs;		/* depth-first, as zpool prints them */
	size_t		 nvdev;
};

struct zfs {
	struct pool	*pools;
	size_t		 n;
};

/*
 * Run `zpool status -j --json-int -p` and parse it; the JSON text stays
 * in b. A system without ZFS, or without pools, gives zero pools.
 * Returns 0, or -1 with errno set (EPROTO: zpool too old for -j).
 */
int	zfs_read(struct buf *b, struct zfs *z);

/* Parse a stored `zpool status -j` report. Returns 0 or -1 (EPROTO). */
int	zfs_parse(const char *json, size_t len, struct zfs *z);

/* Fill vdev.disk: the device node of the disk under each leaf vdev. */
void	zfs_map_disks(struct buf *b, struct zfs *z);

void	zfs_free(struct zfs *z);

#endif
