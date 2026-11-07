/* health.c - judge a disk against the configured rules. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "health.h"

const char *
level_name(enum level l)
{
	return l == LEVEL_CRIT ? "CRITICAL" : l == LEVEL_WARN ? "WARNING" : "OK";
}

/* Record a finding; the worst level is tracked even when the list is full. */
void
health_add(struct health *h, enum level l, const char *fmt, ...)
{
	va_list ap;

	if (l > h->level)
		h->level = l;
	if (h->n == HEALTH_MAX_FINDINGS)
		return;
	va_start(ap, fmt);
	vsnprintf(h->f[h->n].text, sizeof(h->f[h->n].text), fmt, ap);
	va_end(ap);
	h->f[h->n++].level = l;
}

/* Level of v against lim. */
static enum level
over(int64_t v, const struct limit *lim)
{
	if (lim->crit >= 0 && v >= lim->crit)
		return LEVEL_CRIT;
	if (lim->warn >= 0 && v >= lim->warn)
		return LEVEL_WARN;
	return LEVEL_OK;
}

/* Check v against lim and report it as "<what> <v><unit> (limit N)". */
static void
check_limit(struct health *h, const struct limit *lim, int64_t v,
    const char *what, const char *unit)
{
	enum level l = over(v, lim);

	if (l != LEVEL_OK)
		health_add(h, l, "%s %lld%s (limit %lld)", what, (long long)v, unit,
		    (long long)(l == LEVEL_CRIT ? lim->crit : lim->warn));
}

static void
eval_ata(const struct config *c, const struct disk *d, struct health *h)
{
	const struct ata_attr *a;
	uint8_t i;

	/* The disk's own verdict on its normalized values. */
	for (i = 0; i < d->u.ata.nattr; i++) {
		a = &d->u.ata.attr[i];
		if (a->failed == 2)
			health_add(h, a->prefail ? LEVEL_CRIT : LEVEL_WARN,
			    "%s failing now (value %u, threshold %u)",
			    a->name, a->value, a->thresh);
	}
	/* Our thresholds on raw counters. */
	for (i = 0; i < c->nata; i++) {
		if ((a = smart_ata_attr(d, c->ata[i].id)) != NULL)
			check_limit(h, &c->ata[i].lim, a->raw, a->name, "");
	}
}

static void
eval_nvme(const struct config *c, const struct disk *d, struct health *h)
{
	static const char *const bits[] = {
		"available spare below threshold",
		"temperature out of range",
		"reliability degraded",
		"media is read-only",
		"volatile memory backup failed",
		"persistent memory is read-only",
	};
	const struct nvme_health *nv = &d->u.nvme;
	unsigned i;

	for (i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
		if (nv->critical_warning & (1u << i))
			health_add(h, LEVEL_CRIT, "critical warning: %s", bits[i]);
	check_limit(h, &c->nvme_used, nv->percentage_used, "wear", "%");
	check_limit(h, &c->nvme_media_errors, nv->media_errors,
	    "media errors", "");
}

struct limit
health_temp_limit(const struct config *c, const struct disk *d)
{
	struct limit lim = d->proto == PROTO_NVME ? c->temp_nvme :
	    d->rotation_rate == 0 ? c->temp_ssd : c->temp_hdd;

	/* "auto": the limits reported by the disk, if any. */
	if (lim.warn == LIMIT_AUTO)
		lim.warn = d->temp_warn != TEMP_UNKNOWN ? d->temp_warn : LIMIT_OFF;
	if (lim.crit == LIMIT_AUTO)
		lim.crit = d->temp_crit != TEMP_UNKNOWN ? d->temp_crit : LIMIT_OFF;
	return lim;
}

static void
eval_temp(const struct config *c, const struct disk *d, struct health *h)
{
	struct limit lim;

	if (d->temp == TEMP_UNKNOWN)
		return;
	lim = health_temp_limit(c, d);
	check_limit(h, &lim, d->temp, "temperature", "C");
}

/*
 * The ATA log stamps tests with a 16-bit hour counter, so the age is taken
 * modulo 65536: right for any test run in the last 65536 power-on hours
 * (7.5 years).
 */
int64_t
health_selftest_age(const struct disk *d)
{
	int64_t age = d->power_on_hours - d->last_test.hours;

	if (d->proto == PROTO_ATA)
		age = (age % 65536 + 65536) % 65536;
	return age;
}

static void
eval_selftest(const struct config *c, const struct disk *d, struct health *h)
{
	const struct selftest *t = &d->last_test;
	struct limit lim = { c->selftest_max_age, LIMIT_OFF };

	if (t->valid && t->passed == 0) {
		health_add(h, LEVEL_CRIT, "last self-test failed (%s)", t->type);
		return;
	}
	if (c->selftest_max_age == LIMIT_OFF)
		return;
	if (!t->valid)
		health_add(h, LEVEL_WARN, "no self-test in the log");
	else if (d->power_on_hours >= 0 && t->hours >= 0)
		check_limit(h, &lim, health_selftest_age(d), "last self-test", " h ago");
}

void
health_eval(const struct config *c, const struct disk *d, struct health *h)
{
	h->n = 0;
	h->level = LEVEL_OK;

	if (d->state == READ_ERROR) {
		health_add(h, LEVEL_WARN, "cannot read SMART data: %s", d->error);
		return;
	}
	/* Standby disks have no new data; other protocols are not judged. */
	if (d->state != READ_OK || d->proto == PROTO_OTHER)
		return;
	if (d->smart_passed == 0)
		health_add(h, LEVEL_CRIT, "SMART overall health test FAILED");
	if (d->proto == PROTO_ATA)
		eval_ata(c, d, h);
	else
		eval_nvme(c, d, h);
	eval_temp(c, d, h);
	eval_selftest(c, d, h);
}

/* Spares are AVAIL when idle; any other vdev should be ONLINE. */
static int
vdev_ok(const struct vdev *v)
{
	if (v->section == SECTION_SPARES)
		return strcmp(v->state, "AVAIL") == 0;
	return strcmp(v->state, "ONLINE") == 0;
}

/* Pool scrubs: ZFS keeps only the last scan, scrub or resilver. */
static void
eval_scrub(const struct config *c, const struct pool *p, time_t now,
    struct health *h)
{
	char day[16];
	struct tm tm;
	time_t end = (time_t)p->scan_end;

	if (strcmp(p->scan_state, "SCANNING") == 0)
		return;		/* a scrub or resilver is running */
	if (strcmp(p->scan_func, "SCRUB") == 0 && p->scan_errors > 0)
		health_add(h, LEVEL_WARN, "last scrub found %lld errors",
		    (long long)p->scan_errors);
	if (c->scrub_max_age == LIMIT_OFF)
		return;
	if (strcmp(p->scan_func, "SCRUB") != 0 ||
	    strcmp(p->scan_state, "FINISHED") != 0 || end == 0) {
		health_add(h, LEVEL_WARN, "no completed scrub on record%s",
		    strcmp(p->scan_func, "RESILVER") == 0 ?
		    " (last scan was a resilver)" : "");
		return;
	}
	if (now - end > c->scrub_max_age) {
		localtime_r(&end, &tm);
		strftime(day, sizeof(day), "%Y-%m-%d", &tm);
		health_add(h, LEVEL_WARN, "last scrub on %s (limit %lld d)",
		    day, (long long)(c->scrub_max_age / 86400));
	}
}

void
health_pool(const struct config *c, const struct pool *p, time_t now,
    struct health *h)
{
	const struct vdev *v;
	size_t i;
	int leaf;

	h->n = 0;
	h->level = LEVEL_OK;

	if (strcmp(p->state, "ONLINE") != 0)
		health_add(h, LEVEL_CRIT, "pool is %s", p->state);
	for (i = 0; i < p->nvdev; i++) {
		v = &p->vdevs[i];
		/* A mirror or raidz is DEGRADED because of a child, listed. */
		leaf = i + 1 == p->nvdev || p->vdevs[i + 1].depth <= v->depth ||
		    p->vdevs[i + 1].section != v->section;
		if (!vdev_ok(v) && (leaf || strcmp(v->state, "DEGRADED") != 0))
			health_add(h, v->section == SECTION_SPARES &&
			    strcmp(v->state, "INUSE") == 0 ? LEVEL_WARN :
			    LEVEL_CRIT, "%s is %s", v->name, v->state);
		if (v->read_errors + v->write_errors + v->checksum_errors > 0)
			health_add(h, LEVEL_WARN, "%s: %lld read, %lld write, "
			    "%lld checksum errors", v->name,
			    (long long)v->read_errors,
			    (long long)v->write_errors,
			    (long long)v->checksum_errors);
	}
	if (p->data_errors > 0)
		health_add(h, LEVEL_CRIT, "%lld permanent errors in files "
		    "(zpool status -v)", (long long)p->data_errors);
	if (p->size > 0)
		check_limit(h, &c->pool_used, p->alloc * 100 / p->size, "used",
		    "%");
	eval_scrub(c, p, now, h);
}
