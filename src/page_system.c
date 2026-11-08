/* page_system.c - page 2: the machine at a glance, in fastfetch style. */
#define NCURSES_WIDECHAR 1

#include <curses.h>
#include <stdio.h>
#include <string.h>

#include "sysinfo.h"
#include "tui.h"

#define KEY_COLS	13	/* the key column */
#define VALUE_COLS	40	/* values; bars follow them */
#define BAR_COLS	30
#define HIGH		80	/* bars: high from this percent on */
#define CRITICAL	90

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

static void
draw(struct tui *t, int y, int x, int h, int w)
{
	const struct set *s = &t->set;
	const struct pool *p;
	struct sysinfo si;
	char buf[320], size[16], key[160];
	uint64_t disks = 0, used;
	size_t i, hdd = 0, ssd = 0, nvme = 0;
	int r, c, left = x + 4, iw = w - 8;

	sysinfo_read(&si);
	tui_box(t, y, x, h, w, "System", NULL);
	r = y + 2;

	/* user@host, underlined. */
	c = tui_text(t, r, left, iw, COLOR_PAIR(C_ACCENT) | A_BOLD, "%s",
	    si.user);
	c += tui_text(t, r, left + c, iw - c, 0, "@");
	c += tui_text(t, r++, left + c, iw - c, COLOR_PAIR(C_ACCENT) | A_BOLD,
	    "%s", si.host);
	for (i = 0; i < (size_t)c; i++)
		tui_text(t, r, left + (int)i, 1, COLOR_PAIR(C_DIM), "%s",
		    t->sym->h);
	r += 2;

	snprintf(buf, sizeof(buf), "%s%s%s%s", si.ip, si.iface[0] ? " (" : "",
	    si.iface, si.iface[0] ? ")" : "");
	r = row(t, r, left, iw, "Local IP", buf);
	snprintf(buf, sizeof(buf), "%s, %dx%d", si.term, COLS, LINES);
	r = row(t, r, left, iw, "Terminal", buf);
	r = row(t, r, left, iw, "Locale", si.locale);
	r = row(t, r, left, iw, "Shell", si.shell);
	snprintf(buf, sizeof(buf), "%s %s", si.os, si.arch);
	r = row(t, r, left, iw, "OS", buf);
	r = row(t, r, left, iw, "Kernel", si.kernel);
	r = row(t, r, left, iw, "Host", si.board);
	if (si.cpu[0] != '\0')
		snprintf(buf, sizeof(buf), "%s (%d)", si.cpu, si.ncpu);
	else
		snprintf(buf, sizeof(buf), "%d cores", si.ncpu);
	r = row(t, r, left, iw, "CPU", buf);
	r = row(t, r, left, iw, "GPU", si.gpu);
	if (si.mem_total > 0)
		r = usage(t, r, left, iw, "Memory", si.mem_total -
		    (si.mem_avail < si.mem_total ? si.mem_avail : si.mem_total),
		    si.mem_total, 1, HIGH, CRITICAL);
	if (si.swap_total > 0)
		r = usage(t, r, left, iw, "Swap", si.swap_used, si.swap_total,
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
	for (i = 0; i < (size_t)si.nfs && r < y + h - 3; i++) {
		snprintf(key, sizeof(key), "  %s", si.fs[i].mount);
		r = usage(t, r, left, iw, key, si.fs[i].used, si.fs[i].size,
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

	/* Footer: uptime on the left, what the bar colors mean on the right. */
	fmt_uptime(si.uptime, buf, sizeof(buf));
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
	"",
	draw,
	key,
	NULL,
	0,
	NULL,
};
