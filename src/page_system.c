/* page_system.c - page 2: the machine at a glance, in fastfetch style. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sensors.h"
#include "sysinfo.h"
#include "tui.h"
#include "wcurses.h"

#define KEY_COLS	13	/* the key column */
#define VALUE_COLS	40	/* values; bars follow them */
#define BAR_COLS	30
#define HIGH		80	/* bars: high from this percent on */
#define CRITICAL	90
#define TEMP_HIGH	800	/* sensors without limits of their own, 0.1 C */
#define TEMP_CRIT	950

/* Read when the page is first shown and on "r": not at every redraw. */
static struct {
	struct sysinfo	si;
	struct sensors	ss;
	time_t		at;	/* when, for the uptime */
	int		loaded;
} sys;

static void
reread(struct tui *t)
{
	sysinfo_read(&sys.si);
	sensors_read(&sys.ss);
	sys.at = time(NULL);
	sys.loaded = 1;
	if (t != NULL)
		snprintf(t->msg, sizeof(t->msg), "system read again");
}

/* A "key  value" row; returns the next row. */
static int
row(struct tui *t, int y, int x, int w, const char *key, const char *value)
{
	if (value[0] == '\0')
		return y;	/* nothing known: no row */
	tui_text(t, y, x, KEY_COLS - 1, COLOR_PAIR(C_ACCENT) | A_BOLD, "%s",
	    key);
	tui_text(t, y, x + KEY_COLS, w - KEY_COLS, 0, "%s", value);
	return y + 1;
}

/* A usage bar after the value, colored by how full it is. */
static void
bar(struct tui *t, int y, int x, int w, double ratio, int high, int crit)
{
	int fill = (int)(ratio * BAR_COLS + 0.5), pct = (int)(ratio * 100),
	    i, c = x + KEY_COLS + VALUE_COLS;

	if (c + BAR_COLS > x + w)
		return;		/* no room: the value says it already */
	attron(COLOR_PAIR(pct >= crit ? C_CRIT : pct >= high ? C_WARN : C_OK));
	for (i = 0; i < BAR_COLS; i++)
		mvaddstr(y, c + i, i < fill ? t->sym->full : t->sym->empty);
	attroff(A_COLOR);
}

static const char *
gib(uint64_t bytes, char *buf, size_t size)
{
	snprintf(buf, size, "%.1f GiB", bytes / 1073741824.0);
	return buf;
}

/* "used of total (N%)" plus its bar; returns the next row. */
static int
usage(struct tui *t, int y, int x, int w, const char *key, uint64_t used,
    uint64_t total, int binary, int high, int crit)
{
	char u[24], tot[24], line[96];
	double ratio = total > 0 ? (double)used / total : 0;

	if (binary)
		snprintf(line, sizeof(line), "%s of %s (%.0f%%)", gib(used, u,
		    sizeof(u)), gib(total, tot, sizeof(tot)), ratio * 100);
	else
		snprintf(line, sizeof(line), "%s of %s (%.0f%%)",
		    fmt_size(used, u, sizeof(u)), fmt_size(total, tot,
		    sizeof(tot)), ratio * 100);
	y = row(t, y, x, w, key, line);
	bar(t, y - 1, x, w, ratio, high, crit);
	return y;
}

static void
fmt_uptime(int64_t s, char *buf, size_t size)
{
	long long d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;

	if (s < 0)
		snprintf(buf, size, "unknown");
	else
		snprintf(buf, size, "%lld day%s, %lld hour%s, %lld minute%s", d,
		    d == 1 ? "" : "s", h, h == 1 ? "" : "s", m, m == 1 ? "" :
		    "s");
}

/* A temperature, colored by the sensor's own limits or ours. */
static int
temp(struct tui *t, int y, int x, int w, const struct sensor *s)
{
	int crit = s->crit > 0 ? s->crit : TEMP_CRIT;
	int high = s->high > 0 && s->high < crit ? s->high : s->crit > 0 ?
	    crit - 100 : TEMP_HIGH;

	return tui_text(t, y, x, w, COLOR_PAIR(s->temp >= crit ? C_CRIT :
	    s->temp >= high ? C_WARN : C_OK) | A_BOLD, "%d%s",
	    (s->temp + (s->temp < 0 ? -5 : 5)) / 10, t->utf8 ? "°C" : "C");
}

/*
 * One row per chip: "  k10temp     Tctl 52°C  Tccd1 47°C". What does not
 * fit ends as "+N, up to 61°C". Returns the next row.
 */
static int
sensors(struct tui *t, int r, int last, int x, int w)
{
	const struct sensors *ss = &sys.ss;
	const struct sensor *s, *hot;
	char key[32];
	int i, j, c, need;

	if (ss->n == 0)
		return row(t, r, x, w, "Sensors", "none found");
	tui_text(t, r++, x, KEY_COLS, COLOR_PAIR(C_ACCENT) | A_BOLD,
	    "Sensors");
	for (i = 0; i < ss->n && r < last; r++) {
		snprintf(key, sizeof(key), "  %s", ss->s[i].chip);
		tui_text(t, r, x, KEY_COLS - 1, COLOR_PAIR(C_ACCENT) | A_BOLD,
		    "%.*s", KEY_COLS - 1, key);
		c = x + KEY_COLS;
		for (j = i; j < ss->n && ss->s[j].dev == ss->s[i].dev; j++) {
			s = &ss->s[j];
			need = (int)strlen(s->label) + 8;
			if (c + need > x + w - (j + 1 < ss->n &&
			    ss->s[j + 1].dev == s->dev ? 18 : 0))
				break;
			c += tui_text(t, r, c, x + w - c, COLOR_PAIR(C_DIM),
			    "%s ", s->label);
			c += temp(t, r, c, x + w - c, s) + 2;
		}
		/* The rest of the chip, summed up by its hottest. */
		for (hot = NULL, need = 0; j < ss->n &&
		    ss->s[j].dev == ss->s[i].dev; j++, need++)
			if (hot == NULL || ss->s[j].temp > hot->temp)
				hot = &ss->s[j];
		if (hot != NULL) {
			c += tui_text(t, r, c, x + w - c, COLOR_PAIR(C_DIM),
			    "+%d, up to ", need);
			temp(t, r, c, x + w - c, hot);
		}
		i = j;
	}
	return r;
}

static void
draw(struct tui *t, int y, int x, int h, int w)
{
	const struct set *s = &t->set;
	const struct sysinfo *si = &sys.si;
	const struct pool *p;
	char buf[320], size[16], key[160];
	uint64_t disks = 0, used;
	size_t i, hdd = 0, ssd = 0, nvme = 0;
	int r, c, left = x + 4, iw = w - 8;

	if (!sys.loaded)
		reread(NULL);
	tui_box(t, y, x, h, w, "System", NULL);
	r = y + 2;

	/* user@host, underlined. */
	c = tui_text(t, r, left, iw, COLOR_PAIR(C_ACCENT) | A_BOLD, "%s",
	    si->user);
	c += tui_text(t, r, left + c, iw - c, 0, "@");
	c += tui_text(t, r++, left + c, iw - c, COLOR_PAIR(C_ACCENT) | A_BOLD,
	    "%s", si->host);
	for (i = 0; i < (size_t)c; i++)
		tui_text(t, r, left + (int)i, 1, COLOR_PAIR(C_DIM), "%s",
		    t->sym->h);
	r += 2;

	snprintf(buf, sizeof(buf), "%s%s%s%s", si->ip, si->iface[0] ? " (" : "",
	    si->iface, si->iface[0] ? ")" : "");
	r = row(t, r, left, iw, "Local IP", buf);
	snprintf(buf, sizeof(buf), "%s, %dx%d", si->term, COLS, LINES);
	r = row(t, r, left, iw, "Terminal", buf);
	r = row(t, r, left, iw, "Locale", si->locale);
	r = row(t, r, left, iw, "Shell", si->shell);
	snprintf(buf, sizeof(buf), "%s %s", si->os, si->arch);
	r = row(t, r, left, iw, "OS", buf);
	r = row(t, r, left, iw, "Kernel", si->kernel);
	r = row(t, r, left, iw, "Host", si->board);
	if (si->cpu[0] != '\0')
		snprintf(buf, sizeof(buf), "%s (%d)", si->cpu, si->ncpu);
	else
		snprintf(buf, sizeof(buf), "%d cores", si->ncpu);
	r = row(t, r, left, iw, "CPU", buf);
	r = row(t, r, left, iw, "GPU", si->gpu);
	if (si->mem_total > 0)
		r = usage(t, r, left, iw, "Memory", si->mem_total -
		    (si->mem_avail < si->mem_total ? si->mem_avail : si->mem_total),
		    si->mem_total, 1, HIGH, CRITICAL);
	if (si->swap_total > 0)
		r = usage(t, r, left, iw, "Swap", si->swap_used, si->swap_total,
		    1, HIGH, CRITICAL);
	else
		r = row(t, r, left, iw, "Swap", "none");

	/* Physical disks, then filesystems that are not ZFS. */
	for (i = 0; i < s->n; i++) {
		disks += s->disks[i].capacity;
		nvme += s->disks[i].proto == PROTO_NVME;
		ssd += s->disks[i].proto == PROTO_ATA &&
		    s->disks[i].rotation_rate == 0;
		hdd += s->disks[i].proto == PROTO_ATA &&
		    s->disks[i].rotation_rate != 0;
	}
	snprintf(buf, sizeof(buf), "%zu %s %zu HDD, %zu SSD, %zu NVMe %s %s",
	    s->n, t->sym->sep, hdd, ssd, nvme, t->sym->sep,
	    fmt_size(disks, size, sizeof(size)));
	r = row(t, r, left, iw, "Disks", buf);
	for (i = 0; i < (size_t)si->nfs && r < y + h - 3; i++) {
		snprintf(key, sizeof(key), "  %s", si->fs[i].mount);
		r = usage(t, r, left, iw, key, si->fs[i].used, si->fs[i].size,
		    0, HIGH, CRITICAL);
	}

	/* ZFS pools: usable space, at the configured limits. */
	if (s->zfs.n == 0)
		r = row(t, r, left, iw, "ZFS pools", "none");
	else
		tui_text(t, r++, left, KEY_COLS, COLOR_PAIR(C_ACCENT) | A_BOLD,
		    "ZFS pools");
	for (i = 0; i < s->zfs.n && r < y + h - 3; i++) {
		p = &s->zfs.pools[i];
		used = p->size > 0 ? (uint64_t)((double)p->alloc / p->size *
		    p->usable) : 0;
		snprintf(key, sizeof(key), "  %s", p->name);
		r = usage(t, r, left, iw, key, used, (uint64_t)p->usable, 0,
		    (int)t->cfg->pool_used.warn, (int)t->cfg->pool_used.crit);
	}

	/* Below a rule, the temperature sensors. */
	if (r < y + h - 4) {
		for (c = 0; c < iw; c++)
			tui_text(t, r, left + c, 1, COLOR_PAIR(C_DIM), "%s",
			    t->sym->h);
		r = sensors(t, r + 1, y + h - 3, left, iw);
	}

	/* Footer: uptime on the left, what the bar colors mean on the right. */
	fmt_uptime(si->uptime >= 0 ? si->uptime + (time(NULL) - sys.at) : -1,
	    buf, sizeof(buf));
	c = tui_text(t, y + h - 2, left, iw, COLOR_PAIR(C_ACCENT) | A_BOLD,
	    "Uptime ");
	tui_text(t, y + h - 2, left + c, iw - c, 0, "%s", buf);
	c = left + iw - 36;
	if (c > left + 50) {
		c += tui_text(t, y + h - 2, c, 2, COLOR_PAIR(C_OK), "%s ",
		    t->sym->full);
		c += tui_text(t, y + h - 2, c, 8, 0, "normal  ");
		c += tui_text(t, y + h - 2, c, 2, COLOR_PAIR(C_WARN), "%s ",
		    t->sym->full);
		c += tui_text(t, y + h - 2, c, 6, 0, "high  ");
		c += tui_text(t, y + h - 2, c, 2, COLOR_PAIR(C_CRIT), "%s ",
		    t->sym->full);
		tui_text(t, y + h - 2, c, 8, 0, "critical");
	}
}

static int
key(struct tui *t, int ch)
{
	(void)t;
	(void)ch;
	return 0;
}

const struct page page_system = {
	"System",
	"r refresh",
	draw,
	key,
	NULL,
	reread,
	0,
	NULL,
};
