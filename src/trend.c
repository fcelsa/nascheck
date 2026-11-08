/* trend.c - what the history and the snapshots tell, beyond the limits. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "trend.h"

#define DAY		86400
#define WEEK		(7 * DAY)
#define MONTH		(30 * DAY)
#define SNAP_KEEP	600	/* seconds a snapshot list is reused */
#define WEAR_SOON	365	/* days: SSD worn out within a year, warn */
#define FULL_SOON	90	/* days: pool full within three months, warn */
#define MAX_BASE	64	/* counters of a snapshot */

/* Counters that should never grow: growth over a week is a warning. */
static const int never_grow[] = { 5, 187, 188, 197, 198 };

/*
 * SATA SSD wear has no standard id: these attributes, by name, count the
 * life left down from 100 in their normalized value.
 */
static const char *const life_left[] = { "Wear_Leveling_Count",
    "SSD_Life_Left", "Media_Wearout_Indicator", "Percent_Lifetime_Remain",
    "Remaining_Lifetime_Perc", "Percent_Life_Remaining" };

/* And these the data written, by name, with their unit in bytes. */
static const struct { const char *name; uint64_t unit; } written[] = {
	{ "Total_LBAs_Written", 512 },
	{ "Total_Writes_GiB", 1ULL << 30 },
	{ "Host_Writes_GiB", 1ULL << 30 },
	{ "Host_Writes_MiB", 1ULL << 20 },
	{ "Host_Writes_32MiB", 32ULL << 20 },
};

#define NELEM(a)	(sizeof(a) / sizeof((a)[0]))

/* The snapshots, read again when older than SNAP_KEEP. */
static struct {
	struct buf	 b;
	struct snapshots *s;
	int		 n;
	time_t		 at;	/* 0: read them next time */
} snaps;

/* A disk's counters at the start of a period. */
struct base {
	int	 ids[MAX_BASE];
	int64_t	 raw[MAX_BASE];
	int	 value[MAX_BASE];
	int	 n;
	time_t	 when;
};

static int
add_note(struct set *set, const char *subject, const char *fmt, ...)
{
	struct note *n;
	va_list ap;
	void *p;

	if ((p = realloc(set->notes, (set->nnotes + 1) * sizeof(*n))) == NULL)
		return -1;
	set->notes = p;
	n = &set->notes[set->nnotes++];
	snprintf(n->subject, sizeof(n->subject), "%s", subject);
	va_start(ap, fmt);
	vsnprintf(n->text, sizeof(n->text), fmt, ap);
	va_end(ap);
	return 0;
}

/* A counter in the baseline, by id; 0 if absent. */
static int
base_get(const struct base *b, int id, int64_t *raw, int *value)
{
	int i;

	for (i = 0; i < b->n; i++)
		if (b->ids[i] == id) {
			*raw = b->raw[i];
			if (value != NULL)
				*value = b->value[i];
			return 1;
		}
	return 0;
}

/* "40 days", "14 months", "3.5 years". */
static const char *
fmt_days(double days, char *buf, size_t size)
{
	if (days < 60)
		snprintf(buf, size, "%.0f days", days);
	else if (days < 730)
		snprintf(buf, size, "%.0f months", days / 30.4);
	else
		snprintf(buf, size, "%.1f years", days / 365.25);
	return buf;
}

static const struct ata_attr *
attr_named(const struct disk *d, const char *name)
{
	int i;

	for (i = 0; i < d->u.ata.nattr; i++)
		if (strcmp(d->u.ata.attr[i].name, name) == 0)
			return &d->u.ata.attr[i];
	return NULL;
}

/* Counters that grew over the last week; CRC errors on their own. */
static int
week_changes(struct set *set, size_t i, const struct base *b, time_t now)
{
	const struct disk *d = &set->disks[i];
	struct health *h = &set->dh[i];
	const struct ata_attr *a;
	int64_t was;
	size_t k;
	int days = b->n > 0 ? (int)((now - b->when + DAY / 2) / DAY) : 0;

	if (d->proto == PROTO_NVME) {
		if (days >= 1 && base_get(b, NVME_ID_MEDIA_ERRORS, &was, NULL) &&
		    d->u.nvme.media_errors > was)
			health_add(h, LEVEL_WARN, "media errors +%lld in %d days "
			    "(now %lld)", (long long)(d->u.nvme.media_errors -
			    was), days, (long long)d->u.nvme.media_errors);
		return 0;
	}
	for (k = 0; days >= 1 && k < NELEM(never_grow); k++)
		if ((a = smart_ata_attr(d, (uint8_t)never_grow[k])) != NULL &&
		    base_get(b, a->id, &was, NULL) && a->raw > was)
			health_add(h, LEVEL_WARN, "%s +%lld in %d days (now %lld)",
			    a->name, (long long)(a->raw - was), days,
			    (long long)a->raw);

	/* Interface CRC errors: the link (cable, backplane), not the media. */
	if ((a = smart_ata_attr(d, 199)) == NULL || strstr(a->name, "CRC") ==
	    NULL || a->raw <= 0)
		return 0;
	if (days >= 1 && base_get(b, 199, &was, NULL) && a->raw > was) {
		health_add(h, LEVEL_WARN, "CRC errors +%lld in %d days: check "
		    "the cable or backplane", (long long)(a->raw - was), days);
		return 0;
	}
	return add_note(set, short_name(d), "%lld CRC errors%s: a cable or "
	    "backplane fault, not the disk", (long long)a->raw, days >= 1 ?
	    ", none this week" : "");
}

/*
 * SSD and NVMe wear: how much is used, and when it reaches 100% at the
 * pace of the last month; with how much is written a day.
 */
static int
wear(struct set *set, size_t i, const struct base *b, time_t now)
{
	const struct disk *d = &set->disks[i];
	const struct ata_attr *life = NULL, *wr = NULL;
	char text[100] = "", span[24], size[16];
	double days = b->n > 0 ? (double)(now - b->when) / DAY : 0, rate, left;
	int64_t was;
	uint64_t unit = 0, bytes = 0, before = 0;
	int used = -1, used_before = -1, v, n = 0;
	size_t k;

	if (d->proto == PROTO_NVME) {
		used = d->u.nvme.percentage_used;
		if (base_get(b, NVME_ID_USED, &was, NULL))
			used_before = (int)was;
		bytes = (uint64_t)d->u.nvme.data_units_written * 512000;
		if (base_get(b, NVME_ID_UNITS_WRITTEN, &was, NULL))
			before = (uint64_t)was * 512000;
	} else if (d->proto == PROTO_ATA && d->rotation_rate == 0) {
		for (k = 0; life == NULL && k < NELEM(life_left); k++)
			life = attr_named(d, life_left[k]);
		if (life != NULL && life->value > 0 && life->value <= 100) {
			used = 100 - life->value;
			if (base_get(b, life->id, &was, &v) && v > 0 && v <= 100)
				used_before = 100 - v;
		}
		for (k = 0; wr == NULL && k < NELEM(written); k++)
			if ((wr = attr_named(d, written[k].name)) != NULL)
				unit = written[k].unit;
		if (wr != NULL) {
			bytes = (uint64_t)wr->raw * unit;
			if (base_get(b, wr->id, &was, NULL))
				before = (uint64_t)was * unit;
		}
	} else {
		return 0;	/* spinning disks do not wear out this way */
	}

	if (used >= 0) {
		n = snprintf(text, sizeof(text), "worn %d%%", used);
		if (days >= 7 && used_before >= 0 && used > used_before) {
			rate = (used - used_before) / days;
			left = (100 - used) / rate;
			fmt_days(left > 0 ? left : 0, span, sizeof(span));
			if (left < WEAR_SOON) {
				health_add(&set->dh[i], LEVEL_WARN, "worn %d%%, "
				    "100%% in about %s at this pace", used, span);
				return 0;
			}
			n += snprintf(text + n, sizeof(text) - n, ", 100%% in "
			    "about %s at this pace", span);
		} else if (days >= 7 && used_before >= 0) {
			n += snprintf(text + n, sizeof(text) - n, ", no wear "
			    "measured in %.0f days", days);
		}
	}
	if (days >= 1 && before > 0 && bytes > before && n < (int)sizeof(text))
		n += snprintf(text + n, sizeof(text) - n, "%swrites %s a day",
		    n > 0 ? "; " : "", fmt_size((uint64_t)((bytes - before) /
		    days), size, sizeof(size)));
	return n > 0 ? add_note(set, short_name(d), "%s", text) : 0;
}

/* Least squares: the growth of a pool in percent a day. */
static double
slope(const struct series *s)
{
	double mt = 0, mv = 0, num = 0, den = 0;
	size_t i;

	for (i = 0; i < s->n; i++) {
		mt += (double)(s->t[i] - s->t[0]) / s->n;
		mv += s->v[i] / s->n;
	}
	for (i = 0; i < s->n; i++) {
		num += ((double)(s->t[i] - s->t[0]) - mt) * (s->v[i] - mv);
		den += ((double)(s->t[i] - s->t[0]) - mt) *
		    ((double)(s->t[i] - s->t[0]) - mt);
	}
	return den > 0 ? num / den * DAY : 0;
}

/* When a pool fills up at the pace of the last month. */
static int
growth(struct set *set, struct db *db, size_t j, time_t now)
{
	const struct pool *p = &set->zfs.pools[j];
	struct series s = { 0 };
	char span[24];
	double days, perday, used, left;
	int rc = 0;

	if (p->size <= 0 || db_pool_usage(db, p->name, now - MONTH, &s) == -1 ||
	    s.n < 3 || (days = (double)(s.t[s.n - 1] - s.t[0]) / DAY) < 7)
		goto out;
	used = 100.0 * p->alloc / p->size;
	if ((perday = slope(&s)) < 0.01) {
		rc = add_note(set, p->name, "used space steady over %.0f days",
		    days);
		goto out;
	}
	left = (100 - used) / perday;
	fmt_days(left > 0 ? left : 0, span, sizeof(span));
	if (left < FULL_SOON)
		health_add(&set->ph[j], LEVEL_WARN, "full in about %s at the "
		    "pace of the last %.0f days", span, days);
	else
		rc = add_note(set, p->name, "grows %.2f%% a day: full in about "
		    "%s", perday, span);
out:
	series_free(&s);
	return rc;
}

/* Datasets whose newest snapshot is too old: their schedule stopped. */
static int
snapshot_ages(const struct config *c, struct set *set, size_t j, time_t now)
{
	const struct pool *p = &set->zfs.pools[j];
	const struct snapshots *sn;
	size_t len = strlen(p->name);
	int64_t newest = 0;
	char span[24];
	int k, datasets = 0;

	for (k = 0; k < snaps.n; k++) {
		sn = &snaps.s[k];
		if (strncmp(sn->dataset, p->name, len) != 0 ||
		    (sn->dataset[len] != '\0' && sn->dataset[len] != '/'))
			continue;
		datasets++;
		if (sn->newest > newest)
			newest = sn->newest;
		if (c->snapshot_max_age != LIMIT_OFF &&
		    now - sn->newest > c->snapshot_max_age)
			health_add(&set->ph[j], LEVEL_WARN, "%s: newest snapshot "
			    "%lld d old (limit %lld d)", sn->dataset,
			    (long long)((now - sn->newest) / DAY),
			    (long long)(c->snapshot_max_age / DAY));
	}
	if (datasets == 0)
		return 0;
	if (now - newest < 3600)
		snprintf(span, sizeof(span), "%lld min",
		    (long long)((now - newest) / 60));
	else if (now - newest < 2 * DAY)
		snprintf(span, sizeof(span), "%lld h",
		    (long long)((now - newest) / 3600));
	else
		snprintf(span, sizeof(span), "%lld d",
		    (long long)((now - newest) / DAY));
	return add_note(set, p->name, "snapshots of %d dataset%s, the newest "
	    "%s ago", datasets, datasets == 1 ? "" : "s", span);
}

int
trends(const struct config *c, struct set *set, time_t now)
{
	struct base b;
	struct db db;
	size_t i;
	int open = access(c->database, R_OK) == 0 &&
	    db_open_reader(&db, c->database) == 0, rc = 0;

	for (i = 0; i < set->n && rc == 0; i++) {
		if (set->disks[i].state != READ_OK ||
		    set->disks[i].serial[0] == '\0')
			continue;
		b.n = 0;
		if (open && (b.n = db_baseline(&db, set->disks[i].serial,
		    now - WEEK, b.ids, b.raw, b.value, MAX_BASE, &b.when)) < 0)
			b.n = 0;
		rc = week_changes(set, i, &b, now);
		b.n = 0;
		if (rc == 0 && open && (b.n = db_baseline(&db,
		    set->disks[i].serial, now - MONTH, b.ids, b.raw, b.value,
		    MAX_BASE, &b.when)) < 0)
			b.n = 0;
		if (rc == 0)
			rc = wear(set, i, &b, now);
	}
	for (i = 0; i < set->zfs.n && open && rc == 0; i++)
		rc = growth(set, &db, i, now);
	if (open)
		db_close(&db);

	if (set->zfs.n > 0 && (snaps.at == 0 || now - snaps.at > SNAP_KEEP)) {
		free(snaps.s);
		snaps.n = zfs_snapshots(&snaps.b, &snaps.s);
		if (snaps.n < 0)
			snaps.n = 0;
		snaps.at = now;
	}
	for (i = 0; i < set->zfs.n && rc == 0; i++)
		rc = snapshot_ages(c, set, i, now);
	return rc;
}

void
trend_forget(void)
{
	snaps.at = 0;
}

void
trend_free(void)
{
	free(snaps.s);
	buf_free(&snaps.b);
	memset(&snaps, 0, sizeof(snaps));
}
