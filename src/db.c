/* db.c - SQLite storage shared by the daemon (writer) and the readers. */
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#include "db.h"

#define SCHEMA_VERSION	2
#define BUSY_TIMEOUT_MS	5000	/* readers wait for a write in progress */

static const char schema[] =
	"CREATE TABLE IF NOT EXISTS disk ("
	"  id     INTEGER PRIMARY KEY,"
	"  serial TEXT NOT NULL UNIQUE,"	/* identity across reboots */
	"  model  TEXT NOT NULL,"
	"  path   TEXT,"			/* device node, NULL when gone */
	"  seen   INTEGER NOT NULL DEFAULT 1);"
	/* Latest full smartctl report of each disk, replaced each time. */
	"CREATE TABLE IF NOT EXISTS report ("
	"  disk INTEGER PRIMARY KEY REFERENCES disk(id),"
	"  time INTEGER NOT NULL,"
	"  json TEXT NOT NULL);"
	/* History: one row per full report and its counters. */
	"CREATE TABLE IF NOT EXISTS snapshot ("
	"  id    INTEGER PRIMARY KEY,"
	"  disk  INTEGER NOT NULL REFERENCES disk(id),"
	"  time  INTEGER NOT NULL,"
	"  level INTEGER NOT NULL,"
	"  temp  INTEGER,"
	"  hours INTEGER);"
	"CREATE INDEX IF NOT EXISTS snapshot_disk ON snapshot(disk, time);"
	"CREATE TABLE IF NOT EXISTS attribute ("
	"  snapshot INTEGER NOT NULL REFERENCES snapshot(id),"
	"  id       INTEGER NOT NULL,"	/* ATA id, or NVME_ID_* */
	"  raw      INTEGER NOT NULL,"
	"  value    INTEGER,"		/* ATA normalized value */
	"  PRIMARY KEY (snapshot, id)) WITHOUT ROWID;"
	"CREATE TABLE IF NOT EXISTS temp ("
	"  disk    INTEGER NOT NULL REFERENCES disk(id),"
	"  time    INTEGER NOT NULL,"
	"  celsius INTEGER NOT NULL,"
	"  PRIMARY KEY (disk, time)) WITHOUT ROWID;"
	/* Every node of the last scan and how its last read went. */
	"CREATE TABLE IF NOT EXISTS node ("
	"  path  TEXT PRIMARY KEY,"
	"  state INTEGER NOT NULL,"	/* enum read_state */
	"  error TEXT,"
	"  time  INTEGER NOT NULL) WITHOUT ROWID;"
	/* Latest zpool status -j report, and a daily row per pool. */
	"CREATE TABLE IF NOT EXISTS zfs ("
	"  id   INTEGER PRIMARY KEY CHECK (id = 1),"
	"  time INTEGER NOT NULL,"
	"  json TEXT NOT NULL);"
	"CREATE TABLE IF NOT EXISTS pool ("
	"  name   TEXT NOT NULL,"
	"  time   INTEGER NOT NULL,"
	"  state  TEXT NOT NULL,"
	"  alloc  INTEGER NOT NULL,"
	"  size   INTEGER NOT NULL,"
	"  errors INTEGER NOT NULL,"	/* read + write + checksum */
	"  data_errors INTEGER NOT NULL,"
	"  PRIMARY KEY (name, time)) WITHOUT ROWID;"
	"CREATE TABLE IF NOT EXISTS meta ("
	"  key   TEXT PRIMARY KEY,"
	"  value INTEGER) WITHOUT ROWID;";

/* Indexed by enum db_stmt. */
static const char *const sql[ST_COUNT] = {
	[ST_BEGIN] = "BEGIN",
	[ST_COMMIT] = "COMMIT",
	[ST_ROLLBACK] = "ROLLBACK",
	[ST_DISK] = "INSERT INTO disk (serial, model, path, seen)"
	    " VALUES (?1, ?2, ?3, 1) ON CONFLICT (serial) DO UPDATE"
	    " SET model = ?2, path = ?3, seen = 1",
	/* Not RETURNING above: SQLite 3.35, newer than RHEL 9's. */
	[ST_DISK_ID] = "SELECT id FROM disk WHERE serial = ?1",
	[ST_CLEAR_PATH] = "UPDATE disk SET path = NULL"
	    " WHERE path = ?1 AND id <> ?2",
	[ST_REPORT] = "INSERT OR REPLACE INTO report (disk, time, json)"
	    " VALUES (?1, ?2, ?3)",
	[ST_SNAPSHOT] = "INSERT INTO snapshot (disk, time, level, temp, hours)"
	    " VALUES (?1, ?2, ?3, ?4, ?5)",
	[ST_ATTR] = "INSERT INTO attribute (snapshot, id, raw, value)"
	    " VALUES (?1, ?2, ?3, ?4)",
	[ST_TEMP] = "INSERT OR REPLACE INTO temp (disk, time, celsius)"
	    " VALUES (?1, ?2, ?3)",
	[ST_SEEN_RESET] = "UPDATE disk SET seen = 0",
	[ST_SEEN_PATH] = "UPDATE disk SET seen = 1 WHERE path = ?1",
	[ST_DROP_UNSEEN] = "UPDATE disk SET path = NULL WHERE seen = 0",
	[ST_HEARTBEAT] = "INSERT OR REPLACE INTO meta (key, value)"
	    " VALUES ('heartbeat', ?1)",
	[ST_PRUNE] = "DELETE FROM temp WHERE time < ?1",
	[ST_NODE] = "INSERT OR REPLACE INTO node (path, state, error, time)"
	    " VALUES (?1, ?2, ?3, ?4)",
	[ST_NODE_CLEAR] = "DELETE FROM node",
	[ST_ZFS] = "INSERT OR REPLACE INTO zfs (id, time, json)"
	    " VALUES (1, ?1, ?2)",
	[ST_POOL] = "INSERT OR REPLACE INTO pool (name, time, state, alloc,"
	    " size, errors, data_errors) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
};

/* SQLite's message, with the system error behind it when there is one. */
const char *
db_error(const struct db *db)
{
	static char msg[160];
	int err;

	if (db->h == NULL)
		return strerror(errno);
	if ((err = sqlite3_system_errno(db->h)) == 0)
		return sqlite3_errmsg(db->h);
	snprintf(msg, sizeof(msg), "%s: %s", sqlite3_errmsg(db->h),
	    strerror(err));
	return msg;
}

/* Run a prepared statement to completion and make it reusable. */
static int
run(struct db *db, enum db_stmt i)
{
	sqlite3_stmt *st = db->st[i];
	int rc;

	while ((rc = sqlite3_step(st)) == SQLITE_ROW)
		;
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
	return rc == SQLITE_DONE ? 0 : -1;
}

/*
 * Create the database directory and file with the right owner and mode
 * before SQLite opens it: SQLite then gives its -wal and -shm files the
 * same owner and mode, so the group can read all three.
 */
static int
prepare_files(const char *path, gid_t gid)
{
	char dir[PATH_MAX], *slash;
	int fd;

	if (strlen(path) >= sizeof(dir)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(dir, path, strlen(path) + 1);
	if ((slash = strrchr(dir, '/')) != NULL && slash != dir) {
		*slash = '\0';
		if (mkdir(dir, 0750) == -1 && errno != EEXIST)
			return -1;
		if (gid != (gid_t)-1 && chown(dir, (uid_t)-1, gid) == -1)
			return -1;
	}
	if ((fd = open(path, O_RDWR | O_CREAT, 0640)) == -1)
		return -1;
	if (gid != (gid_t)-1 && fchown(fd, (uid_t)-1, gid) == -1) {
		close(fd);
		return -1;
	}
	return close(fd);
}

int
db_open_writer(struct db *db, const char *path, gid_t gid)
{
	sqlite3_stmt *st;
	int i, version = -1;

	memset(db, 0, sizeof(*db));
	if (prepare_files(path, gid) == -1)
		return -1;
	if (sqlite3_open_v2(path, &db->h, SQLITE_OPEN_READWRITE, NULL) !=
	    SQLITE_OK)
		return -1;
	sqlite3_busy_timeout(db->h, BUSY_TIMEOUT_MS);

	/* Refuse a database written by a newer nascheck. */
	if (sqlite3_prepare_v2(db->h, "PRAGMA user_version", -1, &st,
	    NULL) == SQLITE_OK) {
		if (sqlite3_step(st) == SQLITE_ROW)
			version = sqlite3_column_int(st, 0);
		sqlite3_finalize(st);
	}
	if (version < 0 || version > SCHEMA_VERSION) {
		if (version > SCHEMA_VERSION)
			fprintf(stderr, "nascheck: %s: schema version %d is "
			    "newer than this program\n", path, version);
		return -1;
	}

	/* WAL lets readers work while the daemon writes. */
	if (sqlite3_exec(db->h, "PRAGMA journal_mode = WAL;"
	    "PRAGMA synchronous = NORMAL;", NULL, NULL, NULL) != SQLITE_OK ||
	    sqlite3_exec(db->h, schema, NULL, NULL, NULL) != SQLITE_OK ||
	    sqlite3_exec(db->h, "PRAGMA user_version = 2", NULL, NULL,
	    NULL) != SQLITE_OK)
		return -1;
	for (i = 0; i < ST_COUNT; i++)
		if (sqlite3_prepare_v3(db->h, sql[i], -1,
		    SQLITE_PREPARE_PERSISTENT, &db->st[i], NULL) != SQLITE_OK)
			return -1;
	return 0;
}

int
db_open_reader(struct db *db, const char *path)
{
	memset(db, 0, sizeof(*db));
	if (sqlite3_open_v2(path, &db->h, SQLITE_OPEN_READONLY, NULL) !=
	    SQLITE_OK)
		return -1;
	sqlite3_busy_timeout(db->h, BUSY_TIMEOUT_MS);
	return 0;
}

void
db_close(struct db *db)
{
	int i;

	for (i = 0; i < ST_COUNT; i++)
		sqlite3_finalize(db->st[i]);
	sqlite3_close(db->h);
	memset(db, 0, sizeof(*db));
}

int
db_begin(struct db *db)
{
	return run(db, ST_BEGIN);
}

int
db_commit(struct db *db)
{
	return run(db, ST_COMMIT);
}

void
db_rollback(struct db *db)
{
	run(db, ST_ROLLBACK);
}

int
db_seen_reset(struct db *db)
{
	if (run(db, ST_NODE_CLEAR) == -1)
		return -1;
	return run(db, ST_SEEN_RESET);
}

int
db_store_node(struct db *db, const struct disk *d, time_t now)
{
	sqlite3_stmt *st = db->st[ST_NODE];

	sqlite3_bind_text(st, 1, d->path, -1, SQLITE_STATIC);
	sqlite3_bind_int(st, 2, d->state);
	if (d->state == READ_ERROR)
		sqlite3_bind_text(st, 3, d->error, -1, SQLITE_STATIC);
	sqlite3_bind_int64(st, 4, now);
	return run(db, ST_NODE);
}

int
db_seen_path(struct db *db, const char *path)
{
	sqlite3_bind_text(db->st[ST_SEEN_PATH], 1, path, -1, SQLITE_STATIC);
	return run(db, ST_SEEN_PATH);
}

int
db_drop_unseen(struct db *db)
{
	return run(db, ST_DROP_UNSEEN);
}

/* Insert or update the disk row and return its id, -1 on error. */
static sqlite3_int64
disk_id(struct db *db, const struct disk *d)
{
	sqlite3_stmt *st = db->st[ST_DISK];
	sqlite3_int64 id = -1;

	if (d->serial[0] == '\0') {
		errno = EINVAL;		/* nothing to identify the disk by */
		return -1;
	}
	sqlite3_bind_text(st, 1, d->serial, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, d->model, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 3, d->path, -1, SQLITE_STATIC);
	if (run(db, ST_DISK) == -1)
		return -1;
	st = db->st[ST_DISK_ID];
	sqlite3_bind_text(st, 1, d->serial, -1, SQLITE_STATIC);
	if (sqlite3_step(st) == SQLITE_ROW)
		id = sqlite3_column_int64(st, 0);
	if (run(db, ST_DISK_ID) == -1 || id == -1)	/* finish it */
		return -1;

	/* The node now belongs to this disk only. */
	st = db->st[ST_CLEAR_PATH];
	sqlite3_bind_text(st, 1, d->path, -1, SQLITE_STATIC);
	sqlite3_bind_int64(st, 2, id);
	return run(db, ST_CLEAR_PATH) == 0 ? id : -1;
}

static int
store_attr(struct db *db, sqlite3_int64 snap, int id, int64_t raw, int value)
{
	sqlite3_stmt *st = db->st[ST_ATTR];

	sqlite3_bind_int64(st, 1, snap);
	sqlite3_bind_int(st, 2, id);
	sqlite3_bind_int64(st, 3, raw);
	if (value >= 0)
		sqlite3_bind_int(st, 4, value);
	return run(db, ST_ATTR);
}

static int
store_temp(struct db *db, sqlite3_int64 disk, const struct disk *d,
    time_t now)
{
	sqlite3_stmt *st = db->st[ST_TEMP];

	if (d->temp == TEMP_UNKNOWN)
		return 0;
	sqlite3_bind_int64(st, 1, disk);
	sqlite3_bind_int64(st, 2, now);
	sqlite3_bind_int(st, 3, d->temp);
	return run(db, ST_TEMP);
}

int
db_store_report(struct db *db, const struct disk *d, enum level level,
    const char *json, size_t len, time_t now)
{
	const struct nvme_health *nv = &d->u.nvme;
	sqlite3_stmt *st;
	sqlite3_int64 disk, snap;
	uint8_t i;
	int rc = 0;

	if ((disk = disk_id(db, d)) == -1)
		return -1;

	st = db->st[ST_REPORT];
	sqlite3_bind_int64(st, 1, disk);
	sqlite3_bind_int64(st, 2, now);
	sqlite3_bind_text(st, 3, json, (int)len, SQLITE_STATIC);
	if (run(db, ST_REPORT) == -1)
		return -1;

	st = db->st[ST_SNAPSHOT];
	sqlite3_bind_int64(st, 1, disk);
	sqlite3_bind_int64(st, 2, now);
	sqlite3_bind_int(st, 3, level);
	if (d->temp != TEMP_UNKNOWN)
		sqlite3_bind_int(st, 4, d->temp);
	if (d->power_on_hours >= 0)
		sqlite3_bind_int64(st, 5, d->power_on_hours);
	if (run(db, ST_SNAPSHOT) == -1)
		return -1;
	snap = sqlite3_last_insert_rowid(db->h);

	if (d->proto == PROTO_ATA) {
		for (i = 0; i < d->u.ata.nattr && rc == 0; i++)
			rc = store_attr(db, snap, d->u.ata.attr[i].id,
			    d->u.ata.attr[i].raw, d->u.ata.attr[i].value);
	} else if (d->proto == PROTO_NVME) {
		if (store_attr(db, snap, NVME_ID_USED, nv->percentage_used,
		    -1) == -1 ||
		    store_attr(db, snap, NVME_ID_SPARE, nv->available_spare,
		    -1) == -1 ||
		    store_attr(db, snap, NVME_ID_MEDIA_ERRORS, nv->media_errors,
		    -1) == -1 ||
		    store_attr(db, snap, NVME_ID_ERR_LOG, nv->err_log_entries,
		    -1) == -1 ||
		    store_attr(db, snap, NVME_ID_UNSAFE_SHUTDOWNS,
		    nv->unsafe_shutdowns, -1) == -1 ||
		    store_attr(db, snap, NVME_ID_UNITS_WRITTEN,
		    nv->data_units_written, -1) == -1 ||
		    store_attr(db, snap, NVME_ID_CRIT_WARNING,
		    nv->critical_warning, -1) == -1)
			rc = -1;
	}
	if (rc == 0)
		rc = store_temp(db, disk, d, now);
	return rc;
}

int
db_store_temp(struct db *db, const struct disk *d, time_t now)
{
	sqlite3_int64 disk;

	if ((disk = disk_id(db, d)) == -1)
		return -1;
	return store_temp(db, disk, d, now);
}

int
db_store_zfs(struct db *db, const struct zfs *z, const char *json,
    size_t len, int history, time_t now)
{
	const struct pool *p;
	sqlite3_stmt *st = db->st[ST_ZFS];
	int64_t errors;
	size_t i, j;

	sqlite3_bind_int64(st, 1, now);
	sqlite3_bind_text(st, 2, json, (int)len, SQLITE_STATIC);
	if (run(db, ST_ZFS) == -1)
		return -1;
	for (i = 0; history && i < z->n; i++) {
		p = &z->pools[i];
		for (errors = 0, j = 0; j < p->nvdev; j++)
			errors += p->vdevs[j].read_errors +
			    p->vdevs[j].write_errors +
			    p->vdevs[j].checksum_errors;
		st = db->st[ST_POOL];
		sqlite3_bind_text(st, 1, p->name, -1, SQLITE_STATIC);
		sqlite3_bind_int64(st, 2, now);
		sqlite3_bind_text(st, 3, p->state, -1, SQLITE_STATIC);
		sqlite3_bind_int64(st, 4, p->alloc);
		sqlite3_bind_int64(st, 5, p->size);
		sqlite3_bind_int64(st, 6, errors);
		sqlite3_bind_int64(st, 7, p->data_errors);
		if (run(db, ST_POOL) == -1)
			return -1;
	}
	return 0;
}

int
db_get_zfs(struct db *db, struct buf *b)
{
	sqlite3_stmt *st;
	int rc, ret = 0;

	/* A database from before ZFS support has no table: no pools. */
	b->len = 0;
	if (sqlite3_prepare_v2(db->h, "SELECT json FROM zfs WHERE id = 1",
	    -1, &st, NULL) != SQLITE_OK)
		return 0;
	if ((rc = sqlite3_step(st)) == SQLITE_ROW)
		ret = buf_set(b, (const char *)sqlite3_column_text(st, 0),
		    (size_t)sqlite3_column_bytes(st, 0));
	else if (rc != SQLITE_DONE)
		ret = -1;
	sqlite3_finalize(st);
	return ret;
}

int
db_heartbeat(struct db *db, time_t now)
{
	sqlite3_bind_int64(db->st[ST_HEARTBEAT], 1, now);
	return run(db, ST_HEARTBEAT);
}

int
db_prune(struct db *db, time_t before)
{
	sqlite3_bind_int64(db->st[ST_PRUNE], 1, before);
	return run(db, ST_PRUNE);
}

int
db_get_heartbeat(struct db *db, time_t *t)
{
	sqlite3_stmt *st;
	int rc;

	*t = 0;
	if (sqlite3_prepare_v2(db->h,
	    "SELECT value FROM meta WHERE key = 'heartbeat'", -1, &st,
	    NULL) != SQLITE_OK)
		return -1;
	if ((rc = sqlite3_step(st)) == SQLITE_ROW)
		*t = (time_t)sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : -1;
}

int
db_each_report(struct db *db, int (*fn)(void *, const struct db_report *),
    void *ctx)
{
	/*
	 * Disks with a report, then nodes that have none (asleep or not
	 * readable since they appeared), in device order: ada2 before ada10.
	 */
	static const char q[] =
	    "SELECT * FROM ("
	    " SELECT d.path AS path, r.json, r.time, t.time, t.celsius,"
	    "  0, NULL"
	    " FROM disk d JOIN report r ON r.disk = d.id"
	    " LEFT JOIN temp t ON t.disk = d.id AND t.time ="
	    "  (SELECT max(time) FROM temp WHERE disk = d.id)"
	    " WHERE d.path IS NOT NULL"
	    " UNION ALL"
	    " SELECT n.path, NULL, n.time, NULL, NULL, n.state, n.error"
	    " FROM node n WHERE n.state <> 0 AND NOT EXISTS"
	    "  (SELECT 1 FROM disk d JOIN report r ON r.disk = d.id"
	    "   WHERE d.path = n.path))"
	    " ORDER BY length(path), path";
	struct db_report r;
	sqlite3_stmt *st;
	int rc, stopped = 0;

	if (sqlite3_prepare_v2(db->h, q, -1, &st, NULL) != SQLITE_OK)
		return -1;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		r.path = (const char *)sqlite3_column_text(st, 0);
		r.json = (const char *)sqlite3_column_text(st, 1);
		r.len = (size_t)sqlite3_column_bytes(st, 1);
		r.time = (time_t)sqlite3_column_int64(st, 2);
		r.temp_time = (time_t)sqlite3_column_int64(st, 3);
		r.temp = sqlite3_column_int(st, 4);
		r.state = (enum read_state)sqlite3_column_int(st, 5);
		r.error = (const char *)sqlite3_column_text(st, 6);
		if (r.path == NULL)
			continue;
		if (fn(ctx, &r) == -1) {
			stopped = 1;
			break;
		}
	}
	sqlite3_finalize(st);
	return !stopped && rc == SQLITE_DONE ? 0 : -1;
}

void
series_free(struct series *s)
{
	free(s->t);
	free(s->v);
	memset(s, 0, sizeof(*s));
}

/* Run a query with a text and a time parameter returning (time, value). */
static int
series_query(struct db *db, const char *q, const char *key, time_t since,
    struct series *out)
{
	sqlite3_stmt *st;
	void *p;
	int rc;

	out->n = 0;
	if (sqlite3_prepare_v2(db->h, q, -1, &st, NULL) != SQLITE_OK)
		return -1;
	sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
	sqlite3_bind_int64(st, 2, since);
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		if (out->n == out->cap) {
			out->cap = out->cap ? out->cap * 2 : 256;
			if ((p = realloc(out->t, out->cap * sizeof(*out->t))) ==
			    NULL)
				break;
			out->t = p;
			if ((p = realloc(out->v, out->cap * sizeof(*out->v))) ==
			    NULL)
				break;
			out->v = p;
		}
		out->t[out->n] = (time_t)sqlite3_column_int64(st, 0);
		out->v[out->n++] = sqlite3_column_double(st, 1);
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE ? 0 : -1;
}

int
db_temps(struct db *db, const char *serial, time_t since, struct series *out)
{
	return series_query(db,
	    "SELECT t.time, t.celsius FROM temp t JOIN disk d ON d.id = t.disk"
	    " WHERE d.serial = ?1 AND t.time >= ?2 ORDER BY t.time",
	    serial, since, out);
}

int
db_pool_usage(struct db *db, const char *pool, time_t since,
    struct series *out)
{
	return series_query(db,
	    "SELECT time, 100.0 * alloc / size FROM pool"
	    " WHERE name = ?1 AND time >= ?2 AND size > 0 ORDER BY time",
	    pool, since, out);
}

int
db_baseline(struct db *db, const char *serial, time_t since, int *ids,
    int64_t *raw, int max, time_t *when)
{
	static const char q[] =
	    "SELECT s.time, a.id, a.raw FROM attribute a"
	    " JOIN snapshot s ON s.id = a.snapshot"
	    " WHERE a.snapshot = (SELECT s2.id FROM snapshot s2"
	    "  JOIN disk d ON d.id = s2.disk"
	    "  WHERE d.serial = ?1 AND s2.time >= ?2 ORDER BY s2.time LIMIT 1)";
	sqlite3_stmt *st;
	int rc, n = 0;

	*when = 0;
	if (sqlite3_prepare_v2(db->h, q, -1, &st, NULL) != SQLITE_OK)
		return -1;
	sqlite3_bind_text(st, 1, serial, -1, SQLITE_STATIC);
	sqlite3_bind_int64(st, 2, since);
	while ((rc = sqlite3_step(st)) == SQLITE_ROW && n < max) {
		*when = (time_t)sqlite3_column_int64(st, 0);
		ids[n] = sqlite3_column_int(st, 1);
		raw[n++] = sqlite3_column_int64(st, 2);
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE || rc == SQLITE_ROW ? n : -1;
}
