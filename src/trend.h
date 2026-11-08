/* trend.h - what the history and the snapshots tell, beyond the limits. */
#ifndef NASCHECK_TREND_H
#define NASCHECK_TREND_H

#include <time.h>

#include "config.h"
#include "data.h"

/*
 * Look at the history in the database, when there is one, and at the ZFS
 * snapshots: counters that grew in a week, CRC errors (the cable, not the
 * disk), SSD wear and pool growth projected, datasets whose snapshots
 * stopped. Problems become findings of the disk or pool, the rest notes
 * of the set. Returns 0 or -1 (out of memory).
 */
int	trends(const struct config *c, struct set *set, time_t now);

/* The snapshot list is kept a few minutes: read it again next time. */
void	trend_forget(void);

/* Free what trends() keeps between calls. */
void	trend_free(void);

#endif
