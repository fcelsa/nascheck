/* page_disks.c - page 1: disks and pools, details and history graph. */
#define NCURSES_WIDECHAR 1

#include <curses.h>
#include <stdio.h>
#include <string.h>

#include "health.h"
#include "tui.h"

#define DELTA_DAYS	30	/* attribute changes over this period */

/* Selection: disks first, then pools. */
static size_t
items(const struct tui *t)
{
	return t->set.n + t->set.zfs.n;
}

static const struct pool *
sel_pool(const struct tui *t)
{
	return t->sel >= t->set.n && t->sel < items(t) ?
	    &t->set.zfs.pools[t->sel - t->set.n] : NULL;
}

static const struct disk *
sel_disk(const struct tui *t)
{
	return t->sel < t->set.n ? &t->set.disks[t->sel] : NULL;
}

/* Load the graph series and the attribute baseline of the selection. */
static void
load_history(struct tui *t)
{
	const struct disk *d = sel_disk(t);
	const struct pool *p = sel_pool(t);
	time_t now = time(NULL);
	struct db db;

	if (t->hist_valid)
		return;
	t->hist_valid = 1;
	t->hist.n = 0;
	t->nbase = 0;
	if (!t->set.from_db || db_open_reader(&db, t->cfg->database) == -1)
		return;
	if (d != NULL && d->serial[0] != '\0') {
		db_temps(&db, d->serial, now -
		    tui_periods[t->period].seconds, &t->hist);
		t->nbase = db_baseline(&db, d->serial, now - DELTA_DAYS * 86400,
		    t->base_ids, t->base_raw, ATA_MAX_ATTRS, &t->base_time);
		if (t->nbase < 0)
			t->nbase = 0;
	} else if (p != NULL) {
		db_pool_usage(&db, p->name, now -
		    tui_periods[t->period].seconds, &t->hist);
	}
	db_close(&db);
}

/* "+6", "=", or "" when there is no baseline for this counter. */
static const char *
delta(const struct tui *t, int id, int64_t raw, char *buf, size_t size)
{
	int i;

	for (i = 0; i < t->nbase; i++) {
		if (t->base_ids[i] != id)
			continue;
		if (raw == t->base_raw[i])
			return "=";
		snprintf(buf, size, "%+lld", (long long)(raw - t->base_raw[i]));
		return buf;
	}
	return "";
}

/* The pool and parent vdev holding a disk, NULL if none. */
static const struct pool *
find_pool(const struct tui *t, const struct disk *d, const char **parent)
{
	const struct pool *p;
	size_t i, j, k;

	for (i = 0; i < t->set.zfs.n; i++) {
		p = &t->set.zfs.pools[i];
		for (j = 0; j < p->nvdev; j++) {
			if (strcmp(p->vdevs[j].disk, d->path) != 0)
				continue;
			*parent = p->vdevs[j].name;
			for (k = j; k-- > 0;) {
				if (p->vdevs[k].depth < p->vdevs[j].depth) {
					*parent = p->vdevs[k].name;
					break;
				}
			}
			return p;
		}
	}
	return NULL;
}

/* ---- left panel ---------------------------------------------------- */

static void
draw_list(struct tui *t, int y, int x, int h, int w)
{
	const struct set *s = &t->set;
	const struct symbols *sy = t->sym;
	char size[16], temp[16], bar[64];
	int rows = h - 2, iw = w - 2, row, r, sel, attr, k, used;
	size_t i, nrow = s->n + (s->zfs.n > 0 ? s->zfs.n + 2 : 0) + 1;

	tui_box(t, y, x, h, w, "Devices", NULL);
	/* Scroll so that the selection stays visible. */
	sel = (int)(t->sel < s->n ? t->sel + 1 : t->sel + 3);
	if (sel < (int)t->top + 1)
		t->top = sel > 0 ? (size_t)sel - 1 : 0;
	if (sel >= (int)t->top + rows)
		t->top = (size_t)(sel - rows + 1);
	if (nrow <= (size_t)rows)
		t->top = 0;

	for (row = 0; row < (int)nrow; row++) {
		r = row - (int)t->top;
		if (r < 0 || r >= rows)
			continue;
		r += y + 1;
		if (row == 0) {
			tui_text(t, r, x + 1, iw, COLOR_PAIR(C_TITLE) | A_BOLD,
			    " DISKS");
			tui_text(t, r, x + w - 6, 4, COLOR_PAIR(C_DIM),
			    "%sC", sy->deg);
			continue;
		}
		if (row == (int)s->n + 1)
			continue;	/* blank line between the groups */
		if (row == (int)s->n + 2) {
			tui_text(t, r, x + 1, iw, COLOR_PAIR(C_TITLE) | A_BOLD,
			    " ZFS POOLS");
			tui_text(t, r, x + w - 8, 6, COLOR_PAIR(C_DIM),
			    "used");
			continue;
		}
		i = row <= (int)s->n ? (size_t)row - 1 :
		    s->n + (size_t)row - s->n - 3;
		attr = i == t->sel ? COLOR_PAIR(C_SEL) | A_BOLD : 0;
		if (i == t->sel) {
			attron(COLOR_PAIR(C_SEL));
			mvhline(r, x + 1, ' ', iw);
			attroff(COLOR_PAIR(C_SEL));
			tui_text(t, r, x + 1, 1, COLOR_PAIR(C_ACCENT), "%s",
			    sy->sel);
		}
		if (i < s->n) {
			const struct disk *d = &s->disks[i];

			tui_text(t, r, x + 2, 1, d->state == READ_OK ?
			    tui_level_attr(s->dh[i].level) : (int)COLOR_PAIR(C_DIM),
			    "%s", sy->dot);
			snprintf(temp, sizeof(temp), "%s", d->temp ==
			    TEMP_UNKNOWN ? "-" : "");
			if (d->temp != TEMP_UNKNOWN)
				snprintf(temp, sizeof(temp), "%d", d->temp);
			tui_text(t, r, x + 4, iw - 3, attr, "%-7s%-5s%5s",
			    short_name(d), disk_kind(d),
			    d->state == READ_OK ? fmt_size(d->capacity, size,
			    sizeof(size)) : "-");
			tui_text(t, r, x + w - 6, 4, attr, "%3s", temp);
		} else {
			const struct pool *p = &s->zfs.pools[i - s->n];

			used = p->size > 0 ? (int)(p->alloc * 100 / p->size) :
			    0;
			for (bar[0] = '\0', k = 0; k < 5; k++)
				strncat(bar, k < (used + 10) / 20 ? sy->full :
				    sy->empty, sizeof(bar) - strlen(bar) - 1);
			tui_text(t, r, x + 2, 1, tui_level_attr(
			    s->ph[i - s->n].level), "%s", sy->dot);
			tui_text(t, r, x + 4, iw - 3, attr, "%-11.11s%5s ",
			    p->name, fmt_size((uint64_t)p->size, size,
			    sizeof(size)));
			tui_text(t, r, x + 21, 5, COLOR_PAIR(C_OK), "%s", bar);
			tui_text(t, r, x + w - 6, 4, attr, "%3d%%", used);
		}
	}
}

/* ---- right panel ---------------------------------------------------- */

/* Findings, or a short all-clear. */
static int
draw_findings(struct tui *t, const struct health *h, int y, int x, int w,
    int last)
{
	uint8_t i;

	if (y > last)
		return y;
	if (h->n == 0) {
		tui_text(t, y++, x, w, COLOR_PAIR(C_OK), "No problems");
		return y;
	}
	tui_text(t, y++, x, w, COLOR_PAIR(C_TITLE) | A_BOLD, "FINDINGS");
	for (i = 0; i < h->n && y <= last; i++, y++) {
		tui_text(t, y, x, 2, tui_level_attr(h->f[i].level), "%s",
		    h->f[i].level == LEVEL_CRIT ? t->sym->crit : t->sym->warn);
		tui_text(t, y, x + 2, w - 2, 0, "%s", h->f[i].text);
	}
	return y;
}

static void
draw_ata(struct tui *t, const struct disk *d, int y, int x, int w, int last)
{
	const struct config *c = t->cfg;
	const struct ata_attr *a;
	char dbuf[24], head[16] = "";
	int i, j, attr, days, wide;

	if (t->nbase > 0) {
		days = (int)((time(NULL) - t->base_time + 43200) / 86400);
		snprintf(head, sizeof(head), "%s %dd", t->utf8 ? "Δ" : "d",
		    days > 0 ? days : 1);
	}
	/* Narrow screens keep the raw values and their change. */
	wide = w >= 66;
	if (wide)
		tui_text(t, y++, x, w, COLOR_PAIR(C_DIM), "%3s %-24s %3s  %3s  "
		    "%3s %13s %7s", "ID", "ATTRIBUTE", "VAL", "WST", "THR",
		    "RAW", head);
	else
		tui_text(t, y++, x, w, COLOR_PAIR(C_DIM), "%3s %-22s %10s %6s",
		    "ID", "ATTRIBUTE", "RAW", head);
	for (i = 0; i < d->u.ata.nattr && y <= last; i++, y++) {
		a = &d->u.ata.attr[i];
		attr = 0;
		for (j = 0; j < c->nata; j++)
			if (c->ata[j].id == a->id && c->ata[j].lim.warn >= 0 &&
			    a->raw >= c->ata[j].lim.warn)
				attr = COLOR_PAIR(C_WARN);
		if (a->failed == 2)
			attr = COLOR_PAIR(C_CRIT);
		if (wide)
			tui_text(t, y, x, w, attr, "%3u %-24.24s %3u  %3u  "
			    "%3u %13lld %7s", a->id, a->name, a->value,
			    a->worst, a->thresh, (long long)a->raw,
			    delta(t, a->id, a->raw, dbuf, sizeof(dbuf)));
		else
			tui_text(t, y, x, w, attr, "%3u %-22.22s %10lld %6s",
			    a->id, a->name, (long long)a->raw,
			    delta(t, a->id, a->raw, dbuf, sizeof(dbuf)));
	}
}

static void
draw_nvme(struct tui *t, const struct disk *d, int y, int x, int w, int last)
{
	const struct nvme_health *nv = &d->u.nvme;
	char dbuf[24];
	const struct {
		const char	*name;
		int		 id;
		int64_t		 v;
		const char	*unit;
	} rows[] = {
		{ "Critical warning", NVME_ID_CRIT_WARNING, nv->critical_warning,
		    "" },
		{ "Available spare", NVME_ID_SPARE, nv->available_spare, "%" },
		{ "Spare threshold", 0, nv->spare_threshold, "%" },
		{ "Percentage used", NVME_ID_USED, nv->percentage_used, "%" },
		{ "Media errors", NVME_ID_MEDIA_ERRORS, nv->media_errors, "" },
		{ "Error log entries", NVME_ID_ERR_LOG, nv->err_log_entries,
		    "" },
		{ "Unsafe shutdowns", NVME_ID_UNSAFE_SHUTDOWNS,
		    nv->unsafe_shutdowns, "" },
		{ "Data written (GB)", 0, nv->data_units_written * 512 / 1000000,
		    "" },
	};
	size_t i;

	for (i = 0; i < sizeof(rows) / sizeof(rows[0]) && y <= last; i++, y++)
		tui_text(t, y, x, w, 0, "%-22s %12lld%-1s %7s", rows[i].name,
		    (long long)rows[i].v, rows[i].unit, rows[i].id ?
		    delta(t, rows[i].id, rows[i].v, dbuf, sizeof(dbuf)) : "");
}

static void
draw_disk(struct tui *t, const struct disk *d, const struct health *h,
    int y, int x, int w, int last)
{
	const struct symbols *sy = t->sym;
	const struct selftest *st = &d->last_test;
	const struct pool *p;
	const char *parent = "";
	char size[16];
	int c;

	if (d->state == READ_STANDBY) {
		tui_text(t, y++, x, w, 0, "Disk in standby: no report yet.");
		tui_text(t, y, x, w, COLOR_PAIR(C_DIM), "The daemon tries again "
		    "every %lld min and wakes it after %lld days.",
		    (long long)t->cfg->standby_retry / 60,
		    (long long)t->cfg->wake_after / 86400);
		return;
	}
	if (d->state == READ_ERROR) {
		tui_text(t, y, x, w, COLOR_PAIR(C_CRIT), "Read failed: "
		    "%s", d->error);
		return;
	}
	tui_text(t, y++, x, w, 0, "%s %s %s %s %s firmware %s", d->model,
	    sy->sep, disk_kind(d), fmt_size(d->capacity, size, sizeof(size)),
	    sy->sep, d->firmware);
	c = tui_text(t, y, x, w, 0, "powered on %lld h (%.1f years) %s %d%sC",
	    (long long)d->power_on_hours, d->power_on_hours / 8766.0, sy->sep,
	    d->temp, sy->deg);
	if ((p = find_pool(t, d, &parent)) != NULL)
		tui_text(t, y, x + c, w - c, 0, " %s pool %s %s %s", sy->sep,
		    p->name, sy->arrow, parent);
	y++;
	if (st->valid)
		tui_text(t, y++, x, w, COLOR_PAIR(C_DIM), "last self-test: "
		    "%s, %s, %lld h ago", st->type, st->passed == 1 ? "passed" :
		    st->passed == 0 ? "FAILED" : "aborted",
		    (long long)health_selftest_age(d));
	else
		tui_text(t, y++, x, w, COLOR_PAIR(C_DIM), "no self-test in "
		    "the log");
	y = draw_findings(t, h, y + 1, x, w, last);
	y++;
	if (y > last)
		return;
	if (d->proto == PROTO_ATA)
		draw_ata(t, d, y, x, w, last);
	else if (d->proto == PROTO_NVME)
		draw_nvme(t, d, y, x, w, last);
}

static void
draw_pool(struct tui *t, const struct pool *p, const struct health *h,
    int y, int x, int w, int last)
{
	const struct symbols *sy = t->sym;
	const struct set *s = &t->set;
	const struct vdev *v;
	char size[16], day[16];
	struct tm tm;
	time_t end = (time_t)p->scan_end;
	size_t j, k;
	int c, nw, wide;

	/* Last scan: running, done on a day, or none at all. */
	if (strcmp(p->scan_state, "SCANNING") == 0) {
		snprintf(day, sizeof(day), "running");
	} else if (p->scan_func[0] != '\0' && end > 0) {
		localtime_r(&end, &tm);
		strftime(day, sizeof(day), "%Y-%m-%d", &tm);
	} else {
		snprintf(day, sizeof(day), "never");
	}
	tui_text(t, y++, x, w, 0, "%s %s %s %s %lld%% used %s %s %s",
	    p->state, sy->sep, fmt_size((uint64_t)p->size, size, sizeof(size)),
	    sy->sep, p->size > 0 ? (long long)(p->alloc * 100 / p->size) : 0LL,
	    sy->sep, strcmp(p->scan_func, "RESILVER") == 0 ? "resilver" :
	    "scrub", day);
	y = draw_findings(t, h, y + 1, x, w, last) + 1;
	if (y > last)
		return;
	/* Columns: name, state, [read write cksum,] disk, its health. */
	wide = w >= 70;
	nw = w - (wide ? 34 : 22) - 9;
	if (wide)
		tui_text(t, y++, x, w, COLOR_PAIR(C_DIM), "%-*s %-8s %4s %5s "
		    "%5s %-6s", nw, "VDEV", "STATE", "READ", "WRITE", "CKSUM",
		    "DISK");
	else
		tui_text(t, y++, x, w, COLOR_PAIR(C_DIM), "%-*s %-8s %-6s",
		    nw, "VDEV", "STATE", "DISK");
	for (j = 0; j < p->nvdev && y <= last; j++, y++) {
		v = &p->vdevs[j];
		c = x;
		tui_text(t, y, c, nw, 0, "%*s%s", 2 * v->depth, "", v->name);
		c += nw + 1;
		tui_text(t, y, c, 8, COLOR_PAIR(strcmp(v->state, "ONLINE") ==
		    0 || strcmp(v->state, "AVAIL") == 0 ? C_OK : C_CRIT), "%s",
		    v->state);
		c += 9;
		if (wide) {
			tui_text(t, y, c, 17, v->read_errors + v->write_errors +
			    v->checksum_errors > 0 ? (int)COLOR_PAIR(C_WARN) :
			    0, "%4lld %5lld %5lld", (long long)v->read_errors,
			    (long long)v->write_errors,
			    (long long)v->checksum_errors);
			c += 18;
		}
		for (k = 0; v->disk[0] != '\0' && k < s->n; k++) {
			if (strcmp(s->disks[k].path, v->disk) != 0)
				continue;
			tui_text(t, y, c, 6, 0, "%s", short_name(&s->disks[k]));
			tui_text(t, y, c + 7, x + w - c - 7, s->disks[k].state ==
			    READ_OK ? tui_level_attr(s->dh[k].level) :
			    (int)COLOR_PAIR(C_DIM), "%s",
			    level_short(&s->disks[k], &s->dh[k]));
			break;
		}
	}
}

/* ---- graph panel ----------------------------------------------------- */

static void
draw_graph(struct tui *t, int y, int x, int h, int w)
{
	const struct disk *d = sel_disk(t);
	const struct pool *p = sel_pool(t);
	const struct series *s = &t->hist;
	char title[96], tabs[64], label[16];
	time_t now = time(NULL), t0 = now - tui_periods[t->period].seconds;
	double lo, hi, mn = 1e9, mx = -1e9, sum = 0, cur;
	struct limit lim = { LIMIT_OFF, LIMIT_OFF };
	const char *unit = d == NULL ? "%" : t->utf8 ? "°C" : "C";
	struct tm tm;
	int gh = h - 4, gw = w - 2 - 7 - 20, i, k, n = 0;
	time_t gap;

	if (d == NULL && p == NULL)
		return;
	for (tabs[0] = '\0', i = 0; i < tui_nperiods; i++) {
		snprintf(label, sizeof(label), i == t->period ? "[%s] " : "%s ",
		    tui_periods[i].label);
		strncat(tabs, label, sizeof(tabs) - strlen(tabs) - 1);
	}
	tabs[strlen(tabs) - 1] = '\0';
	snprintf(title, sizeof(title), "%s %s %s %s", d != NULL ?
	    "Temperature" : "Used space", d != NULL ? short_name(d) :
	    p->name, t->sym->sep, tui_periods[t->period].label);
	tui_box(t, y, x, h, w, title, tabs);
	if (gh < 2 || gw < 10)
		return;

	for (i = 0; i < (int)s->n; i++) {
		if (s->t[i] < t0)
			continue;
		mn = s->v[i] < mn ? s->v[i] : mn;
		mx = s->v[i] > mx ? s->v[i] : mx;
		sum += s->v[i];
		n++;
	}
	if (n == 0) {
		tui_text(t, y + h / 2, x + 3, w - 6, COLOR_PAIR(C_DIM), "%s",
		    !t->set.from_db ? "History is kept by the daemon, "
		    "which is not running." : "No samples in this period.");
		return;
	}
	if (d != NULL) {
		lim = health_temp_limit(t->cfg, d);
		cur = d->temp != TEMP_UNKNOWN ? d->temp : s->v[s->n - 1];
		/* A readable span around the data, in whole degrees. */
		lo = (int)mn - 2;
		hi = (int)mx + 3;
		if (hi - lo < 10) {
			lo -= (10 - (hi - lo)) / 2;
			hi = lo + 10;
		}
		gap = 3 * t->cfg->temp_interval;
	} else {
		lim = t->cfg->pool_used;
		cur = p->size > 0 ? 100.0 * p->alloc / p->size : 0;
		lo = 0;
		hi = 100;
		gap = 2 * 86400;	/* one row a day */
	}
	tui_graph(t, y + 1, x + 8, gh, gw, s, t0, now, lo, hi,
	    (double)lim.warn, (double)lim.crit, gap);

	/* Axis, labels and statistics. */
	for (i = 0; i < gh; i++) {
		tui_text(t, y + 1 + i, x + 7, 1, COLOR_PAIR(C_DIM), "%s",
		    t->sym->tee);
		if (i == 0 || i == gh - 1 || i == gh / 2)
			tui_text(t, y + 1 + i, x + 1, 6, COLOR_PAIR(C_DIM),
			    "%3.0f%s", hi - (hi - lo) * i / (gh - 1), unit);
	}
	tui_text(t, y + 1 + gh, x + 7, 1, COLOR_PAIR(C_DIM), "%s", t->sym->bl);
	for (i = 0; i < gw; i++)
		tui_text(t, y + 1 + gh, x + 8 + i, 1, COLOR_PAIR(C_DIM), "%s",
		    t->sym->h);
	for (k = 0; k <= 4; k++) {
		time_t at = t0 + (now - t0) * k / 4;

		localtime_r(&at, &tm);
		strftime(label, sizeof(label), tui_periods[t->period].seconds <=
		    86400 ? "%H:%M" : "%d/%m", &tm);
		tui_text(t, y + 2 + gh, x + 8 + (gw - 5) * k / 4, 6,
		    COLOR_PAIR(C_DIM), "%s", label);
	}
	k = x + w - 18;
	tui_text(t, y + 1, k, 17, 0, "now    %3.0f%s", cur, unit);
	tui_text(t, y + 2, k, 17, 0, "min    %3.0f%s", mn, unit);
	tui_text(t, y + 3, k, 17, 0, "max    %3.0f%s", mx, unit);
	tui_text(t, y + 4, k, 17, 0, "avg    %3.0f%s", sum / n, unit);
	if (lim.warn >= 0)
		tui_text(t, y + 6, k, 17, COLOR_PAIR(C_DIM), "limits %lld/%lld",
		    (long long)lim.warn, (long long)lim.crit);
}

/* ---- page ------------------------------------------------------------- */

static void
draw(struct tui *t, int y, int x, int h, int w)
{
	const struct disk *d = sel_disk(t);
	const struct pool *p = sel_pool(t);
	int gh = h >= 34 ? 12 : h >= 26 ? 9 : 0, th = h - gh, lw = w >= 110 ?
	    34 : 30, rw = w - lw;
	char title[96];

	if (t->sel >= items(t))
		t->sel = items(t) > 0 ? items(t) - 1 : 0;
	load_history(t);
	draw_list(t, y, x, th, lw);
	if (d != NULL) {
		snprintf(title, sizeof(title), "%s", short_name(d));
		tui_box(t, y, x + lw, th, rw, title, d->state == READ_OK ?
		    level_name(t->set.dh[t->sel].level) : NULL);
		draw_disk(t, d, &t->set.dh[t->sel], y + 1, x + lw + 2, rw - 4,
		    y + th - 2);
	} else if (p != NULL) {
		tui_box(t, y, x + lw, th, rw, p->name,
		    level_name(t->set.ph[t->sel - t->set.n].level));
		draw_pool(t, p, &t->set.ph[t->sel - t->set.n], y + 1,
		    x + lw + 2, rw - 4, y + th - 2);
	} else {
		tui_box(t, y, x + lw, th, rw, NULL, NULL);
		tui_text(t, y + 1, x + lw + 2, rw - 4, COLOR_PAIR(C_DIM),
		    "No disks found.");
	}
	if (gh > 0)
		draw_graph(t, y + th, x, gh, w);
}

static int
key(struct tui *t, int ch)
{
	size_t n = items(t);

	switch (ch) {
	case KEY_DOWN:
	case 'j':
		if (t->sel + 1 < n)
			t->sel++;
		break;
	case KEY_UP:
	case 'k':
		if (t->sel > 0)
			t->sel--;
		break;
	case KEY_HOME:
	case 'g':
		t->sel = 0;
		break;
	case KEY_END:
	case 'G':
		t->sel = n > 0 ? n - 1 : 0;
		break;
	case 't':
		t->period = (t->period + 1) % tui_nperiods;
		break;
	case 'T':
		t->period = (t->period + tui_nperiods - 1) % tui_nperiods;
		break;
	default:
		return 0;
	}
	t->hist_valid = 0;
	return 1;
}

static void
reload(struct tui *t)
{
	t->hist_valid = 0;
}

const struct page page_disks = {
	"Disks",
	"arrows select  t period",
	draw,
	key,
	reload,
};
