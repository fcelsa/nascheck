/* page_system.c - page 2: the machine at a glance, in fastfetch style. */
#define NCURSES_WIDECHAR 1

#include <sys/stat.h>

#include <curses.h>
#include <stdio.h>
#include <string.h>

#include "sysinfo.h"
#include "tui.h"

#define KEY_COLS	14	/* width of the key column */

/* One "key  value" row; returns the next row. */
static int
row(struct tui *t, int y, int x, int w, const char *key, int attr,
    const char *value)
{
	tui_text(t, y, x, KEY_COLS, COLOR_PAIR(C_ACCENT) | A_BOLD, "%s", key);
	tui_text(t, y, x + KEY_COLS, w - KEY_COLS, attr, "%s", value);
	return y + 1;
}

static void
fmt_duration(int64_t s, char *buf, size_t size)
{
	if (s < 0)
		snprintf(buf, size, "-");
	else if (s >= 86400)
		snprintf(buf, size, "%lld d %lld h", (long long)s / 86400,
		    (long long)s % 86400 / 3600);
	else
		snprintf(buf, size, "%lld h %lld min", (long long)s / 3600,
		    (long long)s % 3600 / 60);
}

/* The machine. */
static int
draw_machine(struct tui *t, int y, int x, int w)
{
	struct sysinfo si;
	char buf[320], bar[96];
	double total, used;
	int i, fill;

	sysinfo_read(&si);
	tui_text(t, y++, x, w, COLOR_PAIR(C_ACCENT) | A_BOLD, "%s", si.host);
	for (i = 0; i < (int)strlen(si.host) && i < w; i++)
		tui_text(t, y, x + i, 1, COLOR_PAIR(C_DIM), "%s", t->sym->h);
	y += 2;

	snprintf(buf, sizeof(buf), "%s %s", si.os, si.arch);
	y = row(t, y, x, w, "OS", 0, buf);
	y = row(t, y, x, w, "Kernel", 0, si.kernel);
	fmt_duration(si.uptime, buf, sizeof(buf));
	y = row(t, y, x, w, "Uptime", 0, buf);
	if (si.load[0] >= 0)
		snprintf(buf, sizeof(buf), "%.2f  %.2f  %.2f", si.load[0],
		    si.load[1], si.load[2]);
	else
		snprintf(buf, sizeof(buf), "-");
	y = row(t, y, x, w, "Load", 0, buf);
	if (si.cpu[0] != '\0')
		snprintf(buf, sizeof(buf), "%d core %s %s", si.ncpu,
		    t->sym->sep, si.cpu);
	else
		snprintf(buf, sizeof(buf), "%d core", si.ncpu);
	y = row(t, y, x, w, "CPU", 0, buf);
	if (si.mem_total > 0) {
		total = si.mem_total / 1073741824.0;
		used = (si.mem_total - (si.mem_avail < si.mem_total ?
		    si.mem_avail : si.mem_total)) / 1073741824.0;
		snprintf(buf, sizeof(buf), "%.1f GiB of %.1f GiB (%.0f%%)",
		    used, total, used * 100 / total);
		y = row(t, y, x, w, "Memory", 0, buf);
		fill = (int)(used / total * 30 + 0.5);
		for (bar[0] = '\0', i = 0; i < 30; i++)
			strncat(bar, i < fill ? t->sym->full : t->sym->empty,
			    sizeof(bar) - strlen(bar) - 1);
		tui_text(t, y++, x + KEY_COLS, w - KEY_COLS, COLOR_PAIR(
		    used / total >= 0.9 ? C_WARN : C_OK), "%s", bar);
	}
	return y;
}

/* What nascheck knows. */
static int
draw_nascheck(struct tui *t, int y, int x, int w)
{
	const struct set *s = &t->set;
	enum level worst = LEVEL_OK, l;
	char buf[320], size[16];
	uint64_t disks = 0, pools = 0;
	size_t i, hdd = 0, ssd = 0, nvme = 0, warns = 0, hot = SIZE_MAX;
	struct stat st;
	time_t now = time(NULL);
	long silence;

	for (i = 0; i < s->n; i++) {
		const struct disk *d = &s->disks[i];

		disks += d->capacity;
		nvme += d->proto == PROTO_NVME;
		ssd += d->proto == PROTO_ATA && d->rotation_rate == 0;
		hdd += d->proto == PROTO_ATA && d->rotation_rate != 0;
		if (d->temp != TEMP_UNKNOWN && (hot == SIZE_MAX ||
		    d->temp > s->disks[hot].temp))
			hot = i;
	}
	for (i = 0; i < s->zfs.n; i++)
		pools += (uint64_t)s->zfs.pools[i].size;
	for (i = 0; i < s->n + s->zfs.n; i++) {
		l = i < s->n ? s->dh[i].level : s->ph[i - s->n].level;
		warns += l != LEVEL_OK;
		worst = l > worst ? l : worst;
	}

	tui_text(t, y++, x, w, COLOR_PAIR(C_ACCENT) | A_BOLD, "nascheck");
	for (i = 0; i < 8; i++)
		tui_text(t, y, x + (int)i, 1, COLOR_PAIR(C_DIM), "%s",
		    t->sym->h);
	y += 2;
	snprintf(buf, sizeof(buf), "%s, %zu disks or pools with findings",
	    level_name(worst), warns);
	y = row(t, y, x, w, "State", tui_level_attr(worst), buf);
	snprintf(buf, sizeof(buf), "%zu %s %zu HDD, %zu SSD, %zu NVMe %s %s",
	    s->n, t->sym->sep, hdd, ssd, nvme, t->sym->sep,
	    fmt_size(disks, size, sizeof(size)));
	y = row(t, y, x, w, "Disks", 0, buf);
	if (hot != SIZE_MAX) {
		snprintf(buf, sizeof(buf), "%s, %d%sC", short_name(
		    &s->disks[hot]), s->disks[hot].temp, t->sym->deg);
		y = row(t, y, x, w, "Hottest", 0, buf);
	}
	snprintf(buf, sizeof(buf), "%zu %s %s", s->zfs.n, t->sym->sep,
	    fmt_size(pools, size, sizeof(size)));
	y = row(t, y, x, w, "ZFS pools", 0, s->zfs.n > 0 ? buf : "none");
	if (s->from_db) {
		silence = daemon_silence(t->cfg, s, now);
		if (silence >= 0)
			snprintf(buf, sizeof(buf), "silent for %ld min", silence);
		else
			snprintf(buf, sizeof(buf), "running, last data %lld "
			    "min ago", (long long)(now - s->heartbeat) / 60);
		y = row(t, y, x, w, "Daemon", silence >= 0 ?
		    COLOR_PAIR(C_CRIT) : 0, buf);
		if (stat(t->cfg->database, &st) == 0)
			snprintf(buf, sizeof(buf), "%s (%s)", t->cfg->database,
			    fmt_size((uint64_t)st.st_size, size, sizeof(size)));
		else
			snprintf(buf, sizeof(buf), "%s", t->cfg->database);
		y = row(t, y, x, w, "Database", 0, buf);
	} else {
		y = row(t, y, x, w, "Daemon", COLOR_PAIR(C_DIM),
		    "not used: reading the disks directly");
	}
	return y;
}

static void
draw(struct tui *t, int y, int x, int h, int w)
{
	int r, i, half = w / 2;

	tui_box(t, y, x, h, w, "System", NULL);
	r = draw_machine(t, y + 2, x + 4, half - 6);
	if (w < 110) {		/* narrow: one column */
		draw_nascheck(t, r + 1, x + 4, w - 8);
	} else {
		draw_nascheck(t, y + 2, x + half + 2, half - 6);
	}
	/* The palette, as fastfetch shows it. */
	for (i = 0; i < 6; i++)
		tui_text(t, y + h - 3, x + 4 + i * 4, 3, COLOR_PAIR(C_HEAT0 + i),
		    "%s%s%s", t->sym->full, t->sym->full, t->sym->full);
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
};
