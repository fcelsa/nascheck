/* health.h - judge a disk against the configured rules. */
#ifndef NASCHECK_HEALTH_H
#define NASCHECK_HEALTH_H

#include <time.h>

#include "config.h"
#include "smart.h"
#include "zfs.h"

/* Ordered by severity; the values are also the check exit codes. */
enum level {
	LEVEL_OK,
	LEVEL_WARN,
	LEVEL_CRIT
};

#define HEALTH_MAX_FINDINGS	16

struct finding {
	char		text[88];
	enum level	level;
};

struct health {
	struct finding	f[HEALTH_MAX_FINDINGS];
	uint8_t		n;	/* findings kept, the extra ones are dropped */
	enum level	level;	/* worst level, extra findings included */
};

/* Evaluate d. A disk in standby has no new data and is OK. */
void		health_eval(const struct config *c, const struct disk *d,
		    struct health *h);

/* Evaluate a ZFS pool at time now (for the scrub age). */
void		health_pool(const struct config *c, const struct pool *p,
		    time_t now, struct health *h);

/* Temperature limits for this kind of disk, "auto" resolved. */
struct limit	health_temp_limit(const struct config *c, const struct disk *d);

/* Add a finding with a printf-style text. */
void		health_add(struct health *h, enum level l, const char *fmt, ...);

/* Power-on hours since the last self-test; the test and the power-on
 * hours must be known. */
int64_t		health_selftest_age(const struct disk *d);

/* "OK", "WARNING", "CRITICAL". */
const char	*level_name(enum level l);

#endif
