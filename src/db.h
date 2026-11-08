/* db.h - SQLite storage shared by the daemon (writer) and the readers. */
#ifndef NASCHECK_DB_H
#define NASCHECK_DB_H

#include <sys/types.h>
#include <stddef.h>
#include <time.h>

#include "health.h"
#include "run.h"
#include "smart.h"
#include "zfs.h"

/* NVMe counters stored in the attribute table, after the ATA ids. */
#define NVME_ID_USED		1001	/* percentage_used */
#define NVME_ID_SPARE		1002	/* available_spare */
#define NVME_ID_MEDIA_ERRORS	1003
#define NVME_ID_ERR_LOG		1004	/* num_err_log_entries */
#define NVME_ID_UNSAFE_SHUTDOWNS 1005
#define NVME_ID_UNITS_WRITTEN	1006	/* data_units_written */
#define NVME_ID_CRIT_WARNING	1007	/* critical_warning */

struct sqlite3;
struct sqlite3_stmt;

enum db_stmt {
	ST_BEGIN, ST_COMMIT, ST_ROLLBACK,
	ST_DISK, ST_DISK_ID, ST_CLEAR_PATH, ST_REPORT, ST_SNAPSHOT, ST_ATTR, ST_TEMP,
	ST_SEEN_RESET, ST_SEEN_PATH, ST_DROP_UNSEEN, ST_HEARTBEAT, ST_PRUNE,
	ST_NODE, ST_NODE_CLEAR, ST_ZFS, ST_POOL,
	ST_COUNT
};

struct db {
	struct sqlite3		*h;
	struct sqlite3_stmt	*st[ST_COUNT];	/* writer only */
};

/*
 * Writer: create the directory, the file and the schema if needed. The
 * directory, the database and its -wal/-shm files belong to group gid
 * (if not -1) and can be read by it. Returns 0 or -1 (see db_error).
 */
int	db_open_writer(struct db *db, const char *path, gid_t gid);

/* Reader: read-only access, which also works without write permission. */
int	db_open_reader(struct db *db, const char *path);

void	db_close(struct db *db);
const char *db_error(const struct db *db);

/* Group the writes of a collection cycle in one transaction. */
int	db_begin(struct db *db);
int	db_commit(struct db *db);
void	db_rollback(struct db *db);

/*
 * A full collection marks the device nodes it finds: db_seen_reset() first,
 * db_seen_path() for each node, db_drop_unseen() at the end clears the
 * node of disks that are gone, so readers no longer show them.
 */
int	db_seen_reset(struct db *db);
int	db_seen_path(struct db *db, const char *path);
int	db_drop_unseen(struct db *db);

/* Record how the last full read of a node went (scan cycles only). */
int	db_store_node(struct db *db, const struct disk *d, time_t now);

/* Store a full report: the raw JSON, a snapshot and its counters. */
int	db_store_report(struct db *db, const struct disk *d, enum level level,
	    const char *json, size_t len, time_t now);

/* Store a temperature sample of a disk read with READ_TEMP. */
int	db_store_temp(struct db *db, const struct disk *d, time_t now);

/*
 * Store the latest `zpool status -j` report; with `history`, also a
 * history row per pool (once per full collection).
 */
int	db_store_zfs(struct db *db, const struct zfs *z, const char *json,
	    size_t len, int history, time_t now);

/* Reader: the latest zpool report into b, empty if there is none. */
int	db_get_zfs(struct db *db, struct buf *b);

/* The daemon is alive: readers compare this time with the clock. */
int	db_heartbeat(struct db *db, time_t now);

/* Delete temperature samples older than `before`. */
int	db_prune(struct db *db, time_t before);

/* Reader: the last update written by the daemon, 0 if none. */
int	db_get_heartbeat(struct db *db, time_t *t);

/* A time series read from the history, oldest first. */
struct series {
	time_t	*t;
	double	*v;
	size_t	 n;
	size_t	 cap;
};

void	series_free(struct series *s);

/* Temperature samples of a disk (by serial) since a time. */
int	db_temps(struct db *db, const char *serial, time_t since,
	    struct series *out);

/* Used space of a pool, in percent, since a time (daily rows). */
int	db_pool_usage(struct db *db, const char *pool, time_t since,
	    struct series *out);

/*
 * Raw counters of the oldest snapshot of a disk taken since `since`, to
 * show how they changed. ids/raw hold up to max entries; returns how many,
 * 0 if there is no such snapshot, -1 on error. *when is its time.
 */
int	db_baseline(struct db *db, const char *serial, time_t since, int *ids,
	    int64_t *raw, int max, time_t *when);

/*
 * One present disk with its latest report and temperature sample, or a
 * node without any report: json is NULL and state tells why.
 */
struct db_report {
	const char	*path;
	const char	*json;
	size_t		 len;
	time_t		 time;		/* of the report, or of the attempt */
	time_t		 temp_time;	/* of the last sample, 0 if none */
	int		 temp;
	enum read_state	 state;		/* when json is NULL */
	const char	*error;		/* when state is READ_ERROR */
};

/* Call fn for each present node, in device order; stop if fn returns -1. */
int	db_each_report(struct db *db,
	    int (*fn)(void *ctx, const struct db_report *r), void *ctx);

#endif
