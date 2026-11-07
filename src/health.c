/* health.c - judge a disk against the configured rules. */
#include <stdarg.h>
#include <stdio.h>

#include "health.h"

const char *
level_name(enum level l)
{
	return l == LEVEL_CRIT ? "CRITICAL" : l == LEVEL_WARN ? "WARNING" : "OK";
}

/* Record a finding; the worst level is tracked even when the list is full. */
static void
add(struct health *h, enum level l, const char *fmt, ...)
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
		add(h, l, "%s %lld%s (limit %lld)", what, (long long)v, unit,
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
			add(h, a->prefail ? LEVEL_CRIT : LEVEL_WARN,
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
			add(h, LEVEL_CRIT, "critical warning: %s", bits[i]);
	check_limit(h, &c->nvme_used, nv->percentage_used, "wear", "%");
	check_limit(h, &c->nvme_media_errors, nv->media_errors,
	    "media errors", "");
}

static void
eval_temp(const struct config *c, const struct disk *d, struct health *h)
{
	struct limit lim;

	if (d->temp == TEMP_UNKNOWN)
		return;
	lim = d->proto == PROTO_NVME ? c->temp_nvme :
	    d->rotation_rate == 0 ? c->temp_ssd : c->temp_hdd;
	/* "auto": the limits reported by the disk, if any. */
	if (lim.warn == LIMIT_AUTO)
		lim.warn = d->temp_warn != TEMP_UNKNOWN ? d->temp_warn : LIMIT_OFF;
	if (lim.crit == LIMIT_AUTO)
		lim.crit = d->temp_crit != TEMP_UNKNOWN ? d->temp_crit : LIMIT_OFF;
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
		add(h, LEVEL_CRIT, "last self-test failed (%s)", t->type);
		return;
	}
	if (c->selftest_max_age == LIMIT_OFF)
		return;
	if (!t->valid)
		add(h, LEVEL_WARN, "no self-test in the log");
	else if (d->power_on_hours >= 0 && t->hours >= 0)
		check_limit(h, &lim, health_selftest_age(d), "last self-test", " h ago");
}

void
health_eval(const struct config *c, const struct disk *d, struct health *h)
{
	h->n = 0;
	h->level = LEVEL_OK;

	if (d->state == READ_ERROR) {
		add(h, LEVEL_WARN, "cannot read SMART data: %s", d->error);
		return;
	}
	/* Standby disks have no new data; other protocols are not judged. */
	if (d->state != READ_OK || d->proto == PROTO_OTHER)
		return;
	if (d->smart_passed == 0)
		add(h, LEVEL_CRIT, "SMART overall health test FAILED");
	if (d->proto == PROTO_ATA)
		eval_ata(c, d, h);
	else
		eval_nvme(c, d, h);
	eval_temp(c, d, h);
	eval_selftest(c, d, h);
}
