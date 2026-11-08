/* data.c - the disks and pools to report on, and shared formatting. */
#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "data.h"
#include "db.h"
#include "trend.h"

/* Decimal units, as printed on disk labels: 4.0T, 960G. */
const char *
fmt_size(uint64_t bytes, char *buf, size_t size)
{
	static const char units[] = "KMGTPE";
	double v = (double)bytes;
	int i = -1;

	if (bytes == 0)
		return "-";
	while (v >= 1000 && i < 5) {
		v /= 1000;
		i++;
	}
	if (i < 0)
		snprintf(buf, size, "%uB", (unsigned)bytes);
	else
		snprintf(buf, size, v < 10 ? "%.1f%c" : "%.0f%c", v, units[i]);
	return buf;
}

const char *
disk_kind(const struct disk *d)
{
	if (d->proto == PROTO_NVME)
		return "NVMe";
	if (d->proto != PROTO_ATA || d->rotation_rate < 0)
		return "-";
	return d->rotation_rate == 0 ? "SSD" : "HDD";
}

const char *
level_short(const struct disk *d, const struct health *h)
{
	if (d->state == READ_STANDBY || (d->state == READ_OK &&
	    d->proto == PROTO_OTHER))
		return "-";
	return h->level == LEVEL_CRIT ? "CRIT" :
	    h->level == LEVEL_WARN ? "WARN" : "OK";
}

/* Device name without the /dev/ prefix. */
const char *
short_name(const struct disk *d)
{
	return strncmp(d->path, "/dev/", 5) == 0 ? d->path + 5 : d->path;
}

void
set_free(struct set *s)
{
	free(s->disks);
	free(s->when);
	free(s->dh);
	free(s->ph);
	free(s->notes);
	zfs_free(&s->zfs);
	memset(s, 0, sizeof(*s));
}

/* Build the disk list from the command line, or scan when it is empty. */
static int
get_disks(struct buf *b, char **names, size_t count, struct disk **disks,
    size_t *n)
{
	size_t i;

	if (count == 0)
		return smart_scan(b, disks, n);
	for (i = 0; i < count; i++) {
		if (strlen(names[i]) >= sizeof((*disks)->path)) {
			fprintf(stderr, "nascheck: %s: name too long\n",
			    names[i]);
			return 2;
		}
	}
	if ((*disks = calloc(count, sizeof(**disks))) == NULL)
		return -1;
	for (i = 0; i < count; i++)
		strcpy((*disks)[i].path, names[i]);
	*n = count;
	return 0;
}

/*
 * Get the disks and read each one. Devices from a scan that are neither
 * ATA nor NVMe (USB sticks, SCSI) are dropped; named ones are kept.
 * Returns 0; 2 on a bad device name (reported); -1 on a fatal error left
 * in errno.
 */
static int
read_live(struct buf *b, char **names, size_t count, struct set *set)
{
	struct disk *d;
	size_t i, kept = 0;
	int rc;

	if ((rc = get_disks(b, names, count, &set->disks, &set->n)) != 0) {
		if (rc == -1 && errno == ENOENT)
			set->problem = "smartctl not found: install smartmontools";
		return rc;
	}
	for (i = 0; i < set->n; i++) {
		d = &set->disks[kept];
		if (i != kept)
			*d = set->disks[i];
		if (smart_read(b, d, READ_FULL) == -1) {
			/* Without smartctl there is nothing more to try. */
			if (errno == ENOENT) {
				set->problem = "smartctl not found: install "
				    "smartmontools";
				return -1;
			}
			fprintf(stderr, "nascheck: %s: %s\n", d->path,
			    errno == EPROTO ? "smartctl output is not JSON" :
			    strerror(errno));
			set->partial = 1;
			continue;
		}
		if (count == 0 && d->state == READ_OK &&
		    d->proto == PROTO_OTHER)
			continue;
		kept++;
	}
	set->n = kept;
	/*
	 * The disk nodes belong to root (and the disk or operator group): a
	 * user finds none, or none that opens. Say so rather than show none.
	 */
	for (i = 0; i < set->n && set->disks[i].state == READ_ERROR; i++)
		;
	if (i == set->n && geteuid() != 0)
		set->problem = "only root can read the disks: use sudo, or run "
		    "the daemon and read its database";
	if (count == 0 && zfs_read(b, &set->zfs) == -1) {
		fprintf(stderr, "nascheck: zpool: %s\n", errno == EPROTO ?
		    "no JSON output (OpenZFS 2.3 or later is required)" :
		    strerror(errno));
		set->partial = 1;
	}
	return 0;
}

struct load_ctx {
	struct set	 *set;
	size_t		  cap;
	char		**names;	/* only these devices, if count > 0 */
	size_t		  count;
};

static int
load_report(void *arg, const struct db_report *r)
{
	struct load_ctx *ctx = arg;
	struct set *set = ctx->set;
	struct disk *d;
	void *p;
	size_t i;

	for (i = 0; i < ctx->count && strcmp(ctx->names[i], r->path) != 0; i++)
		;
	if (ctx->count > 0 && i == ctx->count)
		return 0;
	if (set->n == ctx->cap) {
		ctx->cap = ctx->cap ? ctx->cap * 2 : 16;
		if ((p = realloc(set->disks, ctx->cap * sizeof(*d))) == NULL)
			return -1;
		set->disks = p;
		if ((p = realloc(set->when, ctx->cap * sizeof(time_t))) == NULL)
			return -1;
		set->when = p;
	}
	d = &set->disks[set->n];
	if (r->json == NULL) {
		/* Never read: asleep or not readable since it appeared. */
		memset(d, 0, sizeof(*d));
		d->state = r->state;
		d->temp = d->temp_warn = d->temp_crit = TEMP_UNKNOWN;
		d->power_on_hours = d->rotation_rate = -1;
		d->smart_passed = -1;
		if (r->error != NULL)
			snprintf(d->error, sizeof(d->error), "%s", r->error);
	} else {
		snprintf(d->path, sizeof(d->path), "%s", r->path);
		if (smart_parse(r->json, r->len, 0, d) == -1)
			return 0;	/* a damaged report: skip the disk */
		/* The temperature sample is fresher than the daily report. */
		if (r->temp_time > r->time)
			d->temp = (int16_t)r->temp;
	}
	snprintf(d->path, sizeof(d->path), "%s", r->path);
	set->when[set->n++] = r->time;
	return 0;
}

/* Load the latest report of each present disk and the pools. */
static int
read_db(const struct config *c, struct buf *b, char **names, size_t count,
    struct set *set)
{
	struct load_ctx ctx = { set, 0, names, count };
	struct db db;
	int rc = 0;

	set->from_db = 1;
	if (db_open_reader(&db, c->database) == -1 ||
	    db_get_heartbeat(&db, &set->heartbeat) == -1 ||
	    db_each_report(&db, load_report, &ctx) == -1 ||
	    (count == 0 && db_get_zfs(&db, b) == -1)) {
		fprintf(stderr, "nascheck: %s: %s\n", c->database,
		    db_error(&db));
		fprintf(stderr, "nascheck: readers must be in the group set in "
		    "nascheck.conf; -l reads the disks directly (as root)\n");
		rc = 2;
	} else if (count == 0 && b->len > 0 &&
	    zfs_parse(b->data, b->len, &set->zfs) == -1) {
		set->partial = 1;	/* a damaged pool report */
	}
	db_close(&db);
	return rc;
}

/*
 * The database is used when it exists, unless -l asks for a live read.
 * Only a missing file means "no daemon": a permission problem is reported.
 */
int
get_set(const struct config *c, struct buf *b, char **names, size_t count,
    int live, struct set *set)
{
	struct stat st;
	int rc;

	memset(set, 0, sizeof(*set));
	if (live || (stat(c->database, &st) == -1 && errno == ENOENT))
		rc = read_live(b, names, count, set);
	else
		rc = read_db(c, b, names, count, set);
	if (rc == 0 && set->zfs.n > 0)
		zfs_map_disks(b, &set->zfs);
	return rc;
}

/* Minutes since the daemon last wrote, or -1 when it looks alive. */
long
daemon_silence(const struct config *c, const struct set *set, time_t now)
{
	time_t limit = 2 * c->temp_interval + 60;

	if (!set->from_db || now - set->heartbeat <= limit)
		return -1;
	return set->heartbeat == 0 ? 0 : (long)(now - set->heartbeat) / 60;
}

/*
 * Evaluate every disk and pool into set->dh and set->ph. Data from the
 * database must also be recent enough. Returns 0 or -1 (ENOMEM).
 */
int
evaluate(const struct config *c, struct set *set, time_t now)
{
	time_t age;
	size_t i;

	if ((set->n > 0 && (set->dh = calloc(set->n, sizeof(*set->dh))) ==
	    NULL) || (set->zfs.n > 0 &&
	    (set->ph = calloc(set->zfs.n, sizeof(*set->ph))) == NULL))
		return -1;
	for (i = 0; i < set->n; i++) {
		health_eval(c, &set->disks[i], &set->dh[i]);
		if (!set->from_db || set->disks[i].state != READ_OK)
			continue;
		age = now - set->when[i];
		if (age > c->smart_interval + c->wake_after)
			health_add(&set->dh[i], LEVEL_WARN,
			    "SMART data is %lld h old", (long long)age / 3600);
	}
	for (i = 0; i < set->zfs.n; i++)
		health_pool(c, &set->zfs.pools[i], now, &set->ph[i]);
	return trends(c, set, now);
}

const char *
level_word(enum level l)
{
	return l == LEVEL_CRIT ? "CRIT" : l == LEVEL_WARN ? "WARN" : "OK";
}

