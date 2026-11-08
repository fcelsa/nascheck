/* page_disks.c - page 1: disks and pools, details and history graph. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "health.h"
#include "tui.h"
#include "wcurses.h"

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

/* Datasets come from zfs list, readable by any user. */
static void
load_datasets(struct tui *t)
{
	const struct pool *p = sel_pool(t);

	free(t->ds);
	t->ds = NULL;
	t->nds = 0;
	if (p != NULL && (t->nds = zfs_datasets(&t->dsbuf, p->name,
	    &t->ds)) < 0)
		t->nds = 0;
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
	load_datasets(t);
	if (!t->set.from_db || db_open_reader(&db, t->cfg->database) == -1)
		return;
	if (d != NULL && d->serial[0] != '\0') {
		db_temps(&db, d->serial, now -
		    tui_periods[t->period].seconds, &t->hist);
		t->nbase = db_baseline(&db, d->serial, now - DELTA_DAYS * 86400,
		    t->base_ids, t->base_raw, NULL, ATA_MAX_ATTRS, &t->base_time);
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
			    p->name, fmt_size((uint64_t)p->usable, size,
			    sizeof(size)));
			tui_text(t, r, x + 21, 5, COLOR_PAIR(C_OK), "%s", bar);
			tui_text(t, r, x + w - 6, 4, attr, "%3d%%", used);
		}
	}
}

/* ---- right panel ---------------------------------------------------- */

/* The details panel scrolls: rows before off and past the end are skipped. */
struct view {
	int	y, x, w;	/* first row, column, width */
	int	rows;		/* visible rows */
	int	off;		/* rows scrolled away */
	int	n;		/* rows laid out so far */
};

/* Screen row for the next line, or -1 when it is scrolled out of sight. */
static int
vrow(struct view *v)
{
	int r = v->n++ - v->off;

	return r >= 0 && r < v->rows ? v->y + r : -1;
}

static void
vput(struct tui *t, struct view *v, int attr, const char *fmt,
    const char *text)
{
	int r = vrow(v);

	if (r >= 0)
		tui_text(t, r, v->x, v->w, attr, fmt, text);
}

/*
 * Text from column indent of the next row on, wrapped at spaces; the first
 * row may already hold a label before indent. ASCII text only.
 */
static void
vwrap(struct tui *t, struct view *v, int r, int indent, int attr,
    const char *text)
{
	int w = v->w - indent, len;

	while (*text != '\0') {
		len = (int)strlen(text);
		if (len > w) {
			for (len = w; len > 0 && text[len] != ' '; len--)
				;
			if (len == 0)
				len = w;
		}
		if (r >= 0)
			tui_text(t, r, v->x + indent, w, attr, "%.*s", len, text);
		for (text += len; *text == ' '; text++)
			;
		if (*text != '\0')
			r = vrow(v);
	}
}

/* Findings, or a short all-clear. */
static void
draw_findings(struct tui *t, struct view *v, const struct health *h)
{
	uint8_t i;
	int r;

	if (h->n == 0) {
		vput(t, v, COLOR_PAIR(C_OK), "%s", "No problems");
		return;
	}
	vput(t, v, COLOR_PAIR(C_TITLE) | A_BOLD, "%s", "FINDINGS");
	for (i = 0; i < h->n; i++) {
		if ((r = vrow(v)) < 0)
			continue;
		tui_text(t, r, v->x, 2, tui_level_attr(h->f[i].level), "%s",
		    h->f[i].level == LEVEL_CRIT ? t->sym->crit : t->sym->warn);
		tui_text(t, r, v->x + 2, v->w - 2, 0, "%s", h->f[i].text);
	}
}

static void
draw_ata(struct tui *t, struct view *v, const struct disk *d)
{
	const struct config *c = t->cfg;
	const struct ata_attr *a;
	char dbuf[24], head[16] = "";
	int i, j, attr, days, r, wide = v->w >= 66;

	if (t->nbase > 0) {
		days = (int)((time(NULL) - t->base_time + 43200) / 86400);
		snprintf(head, sizeof(head), "%s %dd", t->utf8 ? "Δ" : "d",
		    days > 0 ? days : 1);
	}
	/* Narrow screens keep the raw values and their change. */
	if ((r = vrow(v)) >= 0) {
		if (wide)
			tui_text(t, r, v->x, v->w, COLOR_PAIR(C_DIM), "%3s %-24s "
			    "%3s  %3s  %3s %13s %7s", "ID", "ATTRIBUTE", "VAL",
			    "WST", "THR", "RAW", head);
		else
			tui_text(t, r, v->x, v->w, COLOR_PAIR(C_DIM),
			    "%3s %-22s %10s %6s", "ID", "ATTRIBUTE", "RAW", head);
	}
	for (i = 0; i < d->u.ata.nattr; i++) {
		if ((r = vrow(v)) < 0)
			continue;
		a = &d->u.ata.attr[i];
		attr = 0;
		for (j = 0; j < c->nata; j++)
			if (c->ata[j].id == a->id && c->ata[j].lim.warn >= 0 &&
			    a->raw >= c->ata[j].lim.warn)
				attr = COLOR_PAIR(C_WARN);
		if (a->failed == 2)
			attr = COLOR_PAIR(C_CRIT);
		if (wide)
			tui_text(t, r, v->x, v->w, attr, "%3u %-24.24s %3u  %3u  "
			    "%3u %13lld %7s", a->id, a->name, a->value,
			    a->worst, a->thresh, (long long)a->raw,
			    delta(t, a->id, a->raw, dbuf, sizeof(dbuf)));
		else
			tui_text(t, r, v->x, v->w, attr, "%3u %-22.22s %10lld "
			    "%6s", a->id, a->name, (long long)a->raw,
			    delta(t, a->id, a->raw, dbuf, sizeof(dbuf)));
	}
}

static void
draw_nvme(struct tui *t, struct view *v, const struct disk *d)
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
	int r;

	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
		if ((r = vrow(v)) >= 0)
			tui_text(t, r, v->x, v->w, 0, "%-22s %12lld%-1s %7s",
			    rows[i].name, (long long)rows[i].v, rows[i].unit,
			    rows[i].id ? delta(t, rows[i].id, rows[i].v, dbuf,
			    sizeof(dbuf)) : "");
}

static void
draw_disk(struct tui *t, struct view *v, const struct disk *d,
    const struct health *h)
{
	const struct symbols *sy = t->sym;
	const struct selftest *st = &d->last_test;
	const struct pool *p;
	const char *parent = "";
	char size[16], line[256];
	int r, c;

	if (d->state == READ_STANDBY) {
		vput(t, v, 0, "%s", "Disk in standby: no report yet.");
		snprintf(line, sizeof(line), "The daemon tries again every %lld "
		    "min and wakes it after %lld days.",
		    (long long)t->cfg->standby_retry / 60,
		    (long long)t->cfg->wake_after / 86400);
		vput(t, v, COLOR_PAIR(C_DIM), "%s", line);
		return;
	}
	if (d->state == READ_ERROR) {
		if (t->set.problem != NULL) {
			vwrap(t, v, vrow(v), 0, COLOR_PAIR(C_WARN) | A_BOLD,
			    t->set.problem);
			vrow(v);
		}
		vwrap(t, v, vrow(v), 0, COLOR_PAIR(C_CRIT), d->error);
		return;
	}
	snprintf(line, sizeof(line), "%s %s %s %s %s firmware %s", d->model,
	    sy->sep, disk_kind(d), fmt_size(d->capacity, size, sizeof(size)),
	    sy->sep, d->firmware);
	vput(t, v, 0, "%s", line);
	if ((r = vrow(v)) >= 0) {
		c = tui_text(t, r, v->x, v->w, 0, "powered on %lld h (%.1f "
		    "years) %s %d%sC", (long long)d->power_on_hours,
		    d->power_on_hours / 8766.0, sy->sep, d->temp, sy->deg);
		if ((p = find_pool(t, d, &parent)) != NULL)
			tui_text(t, r, v->x + c, v->w - c, 0, " %s pool %s %s %s",
			    sy->sep, p->name, sy->arrow, parent);
	}
	if (st->valid)
		snprintf(line, sizeof(line), "last self-test: %s, %s, %lld h ago",
		    st->type, st->passed == 1 ? "passed" : st->passed == 0 ?
		    "FAILED" : "aborted", (long long)health_selftest_age(d));
	else
		snprintf(line, sizeof(line), "no self-test in the log");
	vput(t, v, COLOR_PAIR(C_DIM), "%s", line);
	vrow(v);
	draw_findings(t, v, h);
	vrow(v);
	if (d->proto == PROTO_ATA)
		draw_ata(t, v, d);
	else if (d->proto == PROTO_NVME)
		draw_nvme(t, v, d);
}

/* What the layout survives, top-level vdev by top-level vdev. */
static void
draw_layout(struct tui *t, struct view *v, const struct pool *p)
{
	enum vdev_section last = SECTION_DATA;
	const struct vdev *vd;
	char note[160];
	size_t j, ntop = 0;
	int r;

	for (j = 0; j < p->nvdev; j++)
		ntop += p->vdevs[j].depth == 0 &&
		    p->vdevs[j].section == SECTION_DATA;
	vput(t, v, COLOR_PAIR(C_TITLE) | A_BOLD, "%s", "LAYOUT");
	for (j = 0; j < p->nvdev; j++) {
		vd = &p->vdevs[j];
		/* Each data vdev; logs, cache and spares once each. */
		if (vd->depth != 0 || (vd->section != SECTION_DATA &&
		    vd->section == last))
			continue;
		last = vd->section;
		zfs_layout_note(vd, ntop, note, sizeof(note));
		if ((r = vrow(v)) >= 0)
			tui_text(t, r, v->x, 14, COLOR_PAIR(C_ACCENT), "%s",
			    vd->section == SECTION_DATA ? vd->name :
			    vd->section == SECTION_LOGS ? "logs" :
			    vd->section == SECTION_CACHE ? "cache" : "spares");
		vwrap(t, v, r, 14, 0, note);
	}
	if (ntop > 1)
		vwrap(t, v, vrow(v), 0, COLOR_PAIR(C_DIM), "data is striped "
		    "over these vdevs: losing any one of them loses the pool");
}

/* zpool status layout, with the device behind each leaf and its health. */
static void
draw_vdevs(struct tui *t, struct view *v, const struct pool *p)
{
	const struct set *s = &t->set;
	const struct vdev *vd;
	const char *slash;
	size_t j, k;
	int r, c, wide = v->w >= 70, nw = v->w - (wide ? 50 : 32), leaf = 0;

	if ((r = vrow(v)) >= 0) {
		if (wide)
			tui_text(t, r, v->x, v->w, COLOR_PAIR(C_DIM), "%-*s %-8s "
			    "%4s %5s %5s %-12s %s", nw, "VDEV", "STATE", "READ",
			    "WRITE", "CKSUM", "DEVICE", "DISK");
		else
			tui_text(t, r, v->x, v->w, COLOR_PAIR(C_DIM),
			    "%-*s %-8s %-12s %s", nw, "VDEV", "STATE", "DEVICE",
			    "DISK");
	}
	for (j = 0; j < p->nvdev; j++) {
		if ((r = vrow(v)) < 0)
			continue;
		vd = &p->vdevs[j];
		/* A demo shows "gpt/disk3": labels may hold serial numbers. */
		if (t->demo && vd->children == 0 && (slash = strrchr(vd->name,
		    '/')) != NULL)
			tui_text(t, r, v->x, nw, 0, "%*s%.*sdisk%d",
			    2 * vd->depth, "", (int)(slash - vd->name + 1),
			    vd->name, ++leaf);
		else if (t->demo && vd->children == 0)
			tui_text(t, r, v->x, nw, 0, "%*sdisk%d", 2 * vd->depth,
			    "", ++leaf);
		else
			tui_text(t, r, v->x, nw, 0, "%*s%s", 2 * vd->depth, "",
			    vd->name);
		c = v->x + nw + 1;
		tui_text(t, r, c, 8, COLOR_PAIR(strcmp(vd->state, "ONLINE") ==
		    0 || strcmp(vd->state, "AVAIL") == 0 ? C_OK : C_CRIT), "%s",
		    vd->state);
		c += 9;
		if (wide) {
			tui_text(t, r, c, 17, vd->read_errors + vd->write_errors +
			    vd->checksum_errors > 0 ? (int)COLOR_PAIR(C_WARN) :
			    0, "%4lld %5lld %5lld", (long long)vd->read_errors,
			    (long long)vd->write_errors,
			    (long long)vd->checksum_errors);
			c += 18;
		}
		tui_text(t, r, c, 12, 0, "%s", vd->dev);
		for (k = 0; vd->disk[0] != '\0' && k < s->n; k++) {
			if (strcmp(s->disks[k].path, vd->disk) != 0)
				continue;
			tui_text(t, r, c + 13, v->x + v->w - c - 13,
			    s->disks[k].state == READ_OK ?
			    tui_level_attr(s->dh[k].level) :
			    (int)COLOR_PAIR(C_DIM), "%s %s",
			    short_name(&s->disks[k]),
			    level_short(&s->disks[k], &s->dh[k]));
			break;
		}
	}
}

/* Filesystems and volumes, indented by depth under the pool. */
/* A dataset as listed: its last name component, two spaces per level. */
static const char *
ds_leaf(const char *name, int *indent)
{
	const char *leaf = name;

	for (*indent = 0; *name != '\0'; name++)
		if (*name == '/') {
			*indent += 2;
			leaf = name + 1;
		}
	return leaf;
}

static void
draw_datasets(struct tui *t, struct view *v)
{
	const char *leaf;
	char used[16], refer[16], title[32];
	int i, r, n, indent, nw = 4;

	/* The name column fits the widest name, up to half the row. */
	for (i = 0; i < t->nds; i++) {
		leaf = ds_leaf(t->ds[i].name, &indent);
		if ((n = indent + (int)strlen(leaf)) > nw)
			nw = n;
	}
	if (nw > v->w / 2)
		nw = v->w / 2;

	snprintf(title, sizeof(title), "DATASETS (%d)", t->nds);
	vput(t, v, COLOR_PAIR(C_TITLE) | A_BOLD, "%s", title);
	if ((r = vrow(v)) >= 0)
		tui_text(t, r, v->x, v->w, COLOR_PAIR(C_DIM), "%-*s %7s %7s  %s",
		    nw, "NAME", "USED", "REFER", "MOUNTPOINT");
	for (i = 0; i < t->nds; i++) {
		if ((r = vrow(v)) < 0)
			continue;
		leaf = ds_leaf(t->ds[i].name, &indent);
		tui_text(t, r, v->x, nw, 0, "%*s%s", indent, "", leaf);
		tui_text(t, r, v->x + nw + 1, v->w - nw - 1, COLOR_PAIR(C_DIM),
		    "%7s %7s  %s", fmt_size((uint64_t)t->ds[i].used, used,
		    sizeof(used)), fmt_size((uint64_t)t->ds[i].refer, refer,
		    sizeof(refer)), t->ds[i].mountpoint);
	}
}

static void
draw_pool(struct tui *t, struct view *v, const struct pool *p,
    const struct health *h)
{
	const struct symbols *sy = t->sym;
	char usable[16], raw[16], day[16], line[256];
	struct tm tm;
	time_t end = (time_t)p->scan_end;

	/* Last scan: running, done on a day, or none at all. */
	if (strcmp(p->scan_state, "SCANNING") == 0) {
		snprintf(day, sizeof(day), "running");
	} else if (p->scan_func[0] != '\0' && end > 0) {
		localtime_r(&end, &tm);
		strftime(day, sizeof(day), "%Y-%m-%d", &tm);
	} else {
		snprintf(day, sizeof(day), "never");
	}
	snprintf(line, sizeof(line), "%s %s %s usable (%s raw) %s %lld%% used "
	    "%s %s %s", p->state, sy->sep, fmt_size((uint64_t)p->usable,
	    usable, sizeof(usable)), fmt_size((uint64_t)p->size, raw,
	    sizeof(raw)), sy->sep, p->size > 0 ?
	    (long long)(p->alloc * 100 / p->size) : 0LL, sy->sep,
	    strcmp(p->scan_func, "RESILVER") == 0 ? "resilver" : "scrub", day);
	vput(t, v, 0, "%s", line);
	vrow(v);
	draw_findings(t, v, h);
	vrow(v);
	draw_layout(t, v, p);
	vrow(v);
	draw_vdevs(t, v, p);
	if (t->nds > 0) {
		vrow(v);
		draw_datasets(t, v);
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
	struct view v = { y + 1, x + lw + 2, rw - 4, th - 2, 0, 0 };

	if (t->sel >= items(t))
		t->sel = items(t) > 0 ? items(t) - 1 : 0;
	load_history(t);
	draw_list(t, y, x, th, lw);
	v.off = t->detail_off;
	if (d != NULL) {
		tui_box(t, y, x + lw, th, rw, short_name(d), d->state ==
		    READ_OK ? level_name(t->set.dh[t->sel].level) : NULL);
		draw_disk(t, &v, d, &t->set.dh[t->sel]);
	} else if (p != NULL) {
		tui_box(t, y, x + lw, th, rw, p->name,
		    level_name(t->set.ph[t->sel - t->set.n].level));
		draw_pool(t, &v, p, &t->set.ph[t->sel - t->set.n]);
	} else {
		tui_box(t, y, x + lw, th, rw, NULL, NULL);
		if (t->set.problem != NULL)
			vwrap(t, &v, vrow(&v), 0, COLOR_PAIR(C_WARN) | A_BOLD,
			    t->set.problem);
		else
			vput(t, &v, COLOR_PAIR(C_DIM), "%s", "No disks found.");
	}
	/* Tell when there is more above or below, on the bottom border:
	 * the top one carries the title and the level. */
	t->detail_n = v.n;
	t->detail_rows = v.rows;
	if (v.off > 0)
		tui_text(t, y + th - 1, x + w - 24, 10, COLOR_PAIR(C_ACCENT),
		    " %s PgUp ", t->utf8 ? "▲" : "^");
	if (v.n > v.off + v.rows)
		tui_text(t, y + th - 1, x + w - 14, 10, COLOR_PAIR(C_ACCENT),
		    " %s PgDn ", t->utf8 ? "▼" : "v");
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
		t->hist_valid = 0;
		return 1;
	case 'T':
		t->period = (t->period + tui_nperiods - 1) % tui_nperiods;
		t->hist_valid = 0;
		return 1;
	case KEY_NPAGE:
		/* Scroll the details half a panel, not past their end. */
		t->detail_off += t->detail_rows / 2;
		if (t->detail_off > t->detail_n - t->detail_rows)
			t->detail_off = t->detail_n - t->detail_rows;
		if (t->detail_off < 0)
			t->detail_off = 0;
		return 1;
	case KEY_PPAGE:
		t->detail_off -= t->detail_rows / 2;
		if (t->detail_off < 0)
			t->detail_off = 0;
		return 1;
	default:
		return 0;
	}
	t->hist_valid = 0;
	t->detail_off = 0;
	return 1;
}

static void
reload(struct tui *t)
{
	t->hist_valid = 0;
}

const struct page page_disks = {
	"Disks",
	"arrows select  PgUp/PgDn scroll  t period  r refresh",
	draw,
	key,
	reload,
	tui_refresh_disks,
	0,
	NULL,
};
