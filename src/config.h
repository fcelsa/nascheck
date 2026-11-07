/* config.h - nascheck configuration file. */
#ifndef NASCHECK_CONFIG_H
#define NASCHECK_CONFIG_H

#include <stdint.h>

#include "smart.h"

#define LIMIT_OFF	(-1)	/* no threshold */
#define LIMIT_AUTO	(-2)	/* use the limit reported by the disk */

/* A value at or above warn (crit) raises a warning (critical). */
struct limit {
	int64_t	warn;
	int64_t	crit;
};

/* Threshold on the raw value of an ATA attribute. */
struct ata_rule {
	struct limit	lim;
	uint8_t		id;
};

#define CONFIG_PATH_MAX	256

struct config {
	/* Daemon */
	char		database[CONFIG_PATH_MAX];
	char		socket[CONFIG_PATH_MAX];
	char		group[32];	/* may read the database, "" = root's */
	int64_t		smart_interval;	/* seconds between full reports */
	int64_t		temp_interval;	/* seconds between temperature samples */
	int64_t		standby_retry;	/* retry a sleeping disk after this */
	int64_t		wake_after;	/* then wake it if its report is older */
	int64_t		temp_keep;	/* temperature samples kept this long */

	/* Rules */
	struct limit	temp_hdd;		/* Celsius */
	struct limit	temp_ssd;
	struct limit	temp_nvme;
	struct limit	nvme_used;		/* percentage_used */
	struct limit	nvme_media_errors;
	int64_t		selftest_max_age;	/* power-on hours, or LIMIT_OFF */
	struct ata_rule	ata[ATA_MAX_ATTRS];
	uint8_t		nata;
};

/* Fill c with the built-in defaults. */
void	config_defaults(struct config *c);

/*
 * Read path over the values already in c. A missing file is not an error
 * when `optional` is set. Returns 0, or -1 after reporting the problem
 * on stderr.
 */
int	config_load(struct config *c, const char *path, int optional);

#endif
