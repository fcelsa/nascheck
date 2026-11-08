/* page_health.c - page 4: every problem and note of the machine, worst first. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "trend.h"
#include "tui.h"
#include "wcurses.h"

/* One line: a finding of a disk or pool, or a note (level -1). */
struct item {
	const char	*subject;
	const char	*text;
	int		 level;
	size_t		 order;		/* keeps the order of equal levels */
};

static struct {
	struct item	*v;
	size_t		 n, cap;
	size_t		 top;		/* first line shown */
	int		 rows;		/* lines that fit, from the last draw */
} hp;

static int
add(const char *subject, const char *text, int level)
{
	void *p;

	if (hp.n == hp.cap) {
		hp.cap = hp.cap ? hp.cap * 2 : 64;
		if ((p = realloc(hp.v, hp.cap * sizeof(*hp.v))) == NULL)
			return -1;
		hp.v = p;
	}
	hp.v[hp.n].subject = subject;
	hp.v[hp.n].text = text;
	hp.v[hp.n].level = level;
	hp.v[hp.n].order = hp.n;
	hp.n++;
	return 0;
}

static int
worst_first(const void *a, const void *b)
{
	const struct item *x = a, *y = b;

	if (x->level != y->level)
		return y->level - x->level;
	return x->order < y->order ? -1 : x->order > y->order;
}

/* Gather the findings of every disk and pool, then the notes. */
static void
collect(const struct set *s)
{
	size_t i;
	uint8_t k;

	hp.n = 0;
	for (i = 0; s->dh != NULL && i < s->n; i++)
		for (k = 0; k < s->dh[i].n; k++)
			if (add(short_name(&s->disks[i]), s->dh[i].f[k].text,
			    (int)s->dh[i].f[k].level) == -1)
				return;
	for (i = 0; s->ph != NULL && i < s->zfs.n; i++)
		for (k = 0; k < s->ph[i].n; k++)
			if (add(s->zfs.pools[i].name, s->ph[i].f[k].text,
			    (int)s->ph[i].f[k].level) == -1)
				return;
	for (i = 0; i < s->nnotes; i++)
		if (add(s->notes[i].subject, s->notes[i].text, -1) == -1)
			return;
	qsort(hp.v, hp.n, sizeof(*hp.v), worst_first);
}

static void
draw(struct tui *t, int y, int x, int h, int w)
{
	char right[80];
	size_t i, crit = 0, warn = 0, notes = 0, sw = 6;
	int r, c, left = x + 2, iw = w - 4, hint, first, row;
	const struct item *it;

	collect(&t->set);
	for (i = 0; i < hp.n; i++) {
		crit += hp.v[i].level == LEVEL_CRIT;
		warn += hp.v[i].level == LEVEL_WARN;
		notes += hp.v[i].level < 0;
		if (strlen(hp.v[i].subject) > sw)
			sw = strlen(hp.v[i].subject);
	}
	if (sw > 16)
		sw = 16;
	snprintf(right, sizeof(right), "%zu critical %s %zu warning%s %s %zu "
	    "note%s", crit, t->sym->sep, warn, warn == 1 ? "" : "s",
	    t->sym->sep, notes, notes == 1 ? "" : "s");
	tui_box(t, y, x, h, w, "Health", right);

	/* Without the daemon's history, say what is missing. */
	hint = access(t->cfg->database, R_OK) != 0;
	first = crit + warn == 0;	/* a line says all is fine */
	hp.rows = h - 2 - (hint ? 2 : 0) - first;
	if (hp.rows < 1)
		return;
	if (hp.top + (size_t)hp.rows > hp.n)
		hp.top = hp.n > (size_t)hp.rows ? hp.n - (size_t)hp.rows : 0;

	if (first && t->set.problem != NULL)
		tui_text(t, y + 1, left, iw, COLOR_PAIR(C_WARN) | A_BOLD, "%s",
		    t->set.problem);
	else if (first)
		tui_text(t, y + 1, left, iw, COLOR_PAIR(C_OK) | A_BOLD, "%s",
		    "No problems: every disk and pool is fine.");
	for (r = 0; r < hp.rows && hp.top + (size_t)r < hp.n; r++) {
		it = &hp.v[hp.top + (size_t)r];
		row = y + 1 + first + r;
		c = left;
		if (it->level < 0) {
			c += tui_text(t, row, c, 2, COLOR_PAIR(C_DIM),
			    "%s", t->sym->dot);
			c += tui_text(t, row, c + 1, 5, COLOR_PAIR(C_DIM),
			    "note") + 1;
		} else {
			c += tui_text(t, row, c, 2,
			    tui_level_attr((enum level)it->level), "%s",
			    it->level == LEVEL_CRIT ? t->sym->crit :
			    t->sym->warn);
			c += tui_text(t, row, c + 1, 5,
			    tui_level_attr((enum level)it->level) | A_BOLD,
			    "%s", level_word((enum level)it->level)) + 1;
		}
		c = left + 8;
		tui_text(t, row, c, (int)sw, COLOR_PAIR(C_ACCENT) |
		    A_BOLD, "%s", it->subject);
		c += (int)sw + 2;
		tui_text(t, row, c, left + iw - c, it->level < 0 ?
		    (int)COLOR_PAIR(C_DIM) : 0, "%s", it->text);
	}
	if (hint)
		tui_text(t, y + h - 2, left, iw, COLOR_PAIR(C_DIM), "%s",
		    "Trends need the history kept by the daemon.");

	/* More above or below. */
	if (hp.top > 0)
		tui_text(t, y + h - 1, x + w - 24, 10, COLOR_PAIR(C_ACCENT),
		    " %s PgUp ", t->utf8 ? "▲" : "^");
	if (hp.top + (size_t)hp.rows < hp.n)
		tui_text(t, y + h - 1, x + w - 14, 10, COLOR_PAIR(C_ACCENT),
		    " %s PgDn ", t->utf8 ? "▼" : "v");
}

static int
key(struct tui *t, int ch)
{
	size_t page = hp.rows > 1 ? (size_t)hp.rows - 1 : 1;

	(void)t;
	switch (ch) {
	case KEY_DOWN:
	case 'j':
		hp.top++;	/* draw() keeps it in range */
		return 1;
	case KEY_UP:
	case 'k':
		if (hp.top > 0)
			hp.top--;
		return 1;
	case KEY_NPAGE:
		hp.top += page;
		return 1;
	case KEY_PPAGE:
		hp.top = hp.top > page ? hp.top - page : 0;
		return 1;
	case KEY_HOME:
	case 'g':
		hp.top = 0;
		return 1;
	case KEY_END:
	case 'G':
		hp.top = hp.n;
		return 1;
	}
	return 0;
}

/* "r": read the disks and pools again, the snapshots too. */
static void
reread(struct tui *t)
{
	trend_forget();
	tui_refresh_disks(t);
}

static void
fini(struct tui *t)
{
	(void)t;
	free(hp.v);
	memset(&hp, 0, sizeof(hp));
	trend_free();
}

const struct page page_health = {
	"Health",
	"arrows scroll  r refresh",
	draw,
	key,
	NULL,
	reread,
	0,
	fini,
};
