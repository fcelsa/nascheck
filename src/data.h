/* data.h - the disks and pools to report on, and shared formatting. */
#ifndef NASCHECK_DATA_H
#define NASCHECK_DATA_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "config.h"
#include "health.h"
#include "run.h"
#include "smart.h"
#include "zfs.h"

/* The disks and pools to report on, read live or from the database. */
struct set {
	struct disk	*disks;
	time_t		*when;		/* report times, from the database */
	size_t		 n;
	struct zfs	 zfs;		/* pools, when no device was named */
	struct health	*dh;		/* evaluation of each disk */
	struct health	*ph;		/* and of each pool */
	time_t		 heartbeat;	/* last update by the daemon */
	int		 from_db;	/* loaded from the database */
	int		 partial;	/* some data could not be read */
};

/*
 * Fill set from the daemon's database when it exists, unless live is set,
 * or by reading the disks (root). Pools only when no device is named.
 * Returns 0; 2 on an error already reported (bad name, unreadable
 * database); -1 on a fatal error left in errno (smartctl missing).
 */
int		 get_set(const struct config *c, struct buf *b, char **names,
		    size_t count, int live, struct set *set);

/* Evaluate every disk and pool into set->dh and set->ph. 0 or -1. */
int		 evaluate(const struct config *c, struct set *set, time_t now);

void		 set_free(struct set *s);

/* Minutes since the daemon last wrote, or -1 when it looks alive. */
long		 daemon_silence(const struct config *c, const struct set *set,
		    time_t now);

/* Decimal units, as printed on disk labels: 4.0T, 960G. */
const char	*fmt_size(uint64_t bytes, char *buf, size_t size);

const char	*disk_kind(const struct disk *d);	/* HDD, SSD, NVMe */
const char	*short_name(const struct disk *d);	/* without /dev/ */
const char	*level_word(enum level l);		/* OK, WARN, CRIT */

/* Like level_word(), "-" for disks without an evaluation. */
const char	*level_short(const struct disk *d, const struct health *h);

#endif
