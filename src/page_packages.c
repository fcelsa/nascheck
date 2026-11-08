/* page_packages.c - page 3: installed packages, with a live search. */
#define NCURSES_WIDECHAR 1

#include <ctype.h>
#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pkgs.h"
#include "tui.h"

#define QUERY_MAX	64
#define WORDS_MAX	8
#define NAME_COLS	40	/* name and version column */
#define DETAIL_ROWS	4	/* the selected package, at the bottom */

/* The page keeps its own state: nothing else needs it. */
static struct {
	struct packages	 ps;
	int		 loaded;
	char		 query[QUERY_MAX];
	size_t		 qlen;
	char		 wbuf[QUERY_MAX];	/* the words, lowercased */
	const char	*words[WORDS_MAX];
	int		 nwords;
	size_t		*hits;		/* indexes into ps.p, best first */
	int		*score;		/* by package index */
	size_t		 nhits;
	size_t		 sel;		/* index into hits */
	size_t		 top;		/* first listed hit */
} pg;

/* Case-insensitive search for an ASCII needle; NULL if absent. */
static const char *
find(const char *hay, const char *needle)
{
	size_t n = strlen(needle), i;

	for (; *hay != '\0'; hay++) {
		for (i = 0; i < n && hay[i] != '\0' &&
		    tolower((unsigned char)hay[i]) == needle[i]; i++)
			;
		if (i == n)
			return hay;
	}
	return NULL;
}

/* The letters of word in order, gaps allowed: zfst finds zfs-stats. */
static int
subsequence(const char *s, const char *word)
{
	for (; *s != '\0' && *word != '\0'; s++)
		if (tolower((unsigned char)*s) == *word)
			word++;
	return *word == '\0';
}

/* How well one word matches a package: 0 means not at all. */
static int
score_word(const struct package *p, const char *w)
{
	const char *at = find(p->name, w);

	if (at == p->name)
		return p->name[strlen(w)] == '\0' ? 150 : 100;	/* prefix */
	if (at != NULL)
		return 60;
	if (find(p->comment, w) != NULL)
		return 20;
	return subsequence(p->name, w) ? 5 : 0;
}

static int
by_score(const void *a, const void *b)
{
	size_t i = *(const size_t *)a, j = *(const size_t *)b;

	if (pg.score[i] != pg.score[j])
		return pg.score[j] - pg.score[i];
	return strcmp(pg.ps.p[i].name, pg.ps.p[j].name);
}

/* Split the query into words and rank the packages that match them all. */
static void
search(void)
{
	char *w, *save;
	size_t i;
	int k, s, total;

	memcpy(pg.wbuf, pg.query, pg.qlen + 1);
	for (i = 0; i < pg.qlen; i++)
		pg.wbuf[i] = (char)tolower((unsigned char)pg.wbuf[i]);
	pg.nwords = 0;
	for (w = strtok_r(pg.wbuf, " ", &save); w != NULL &&
	    pg.nwords < WORDS_MAX; w = strtok_r(NULL, " ", &save))
		pg.words[pg.nwords++] = w;

	pg.nhits = 0;
	for (i = 0; i < pg.ps.n; i++) {
		for (total = 0, k = 0; k < pg.nwords; k++) {
			if ((s = score_word(&pg.ps.p[i], pg.words[k])) == 0)
				break;
			total += s;
		}
		if (k < pg.nwords)
			continue;
		pg.score[i] = total;
		pg.hits[pg.nhits++] = i;
	}
	if (pg.nwords > 0)
		qsort(pg.hits, pg.nhits, sizeof(*pg.hits), by_score);
	pg.sel = pg.top = 0;
}

static void
load(void)
{
	pg.loaded = 1;
	if (pkgs_load(&pg.ps) == -1 ||
	    (pg.hits = calloc(pg.ps.n + 1, sizeof(*pg.hits))) == NULL ||
	    (pg.score = calloc(pg.ps.n + 1, sizeof(*pg.score))) == NULL) {
		pkgs_free(&pg.ps);
		return;
	}
	search();
}

/*
 * Text clipped to w columns, with what matches the query highlighted.
 * Returns the columns written.
 */
static int
marked(struct tui *t, int y, int x, int w, int attr, const char *text)
{
	char mark[256];
	const char *at;
	size_t len = strlen(text), i, j, wl;
	int k, cols = 0, c;

	if (len >= sizeof(mark))
		len = sizeof(mark) - 1;
	memset(mark, 0, len);
	for (k = 0; k < pg.nwords; k++) {
		wl = strlen(pg.words[k]);
		for (at = text; (at = find(at, pg.words[k])) != NULL &&
		    (size_t)(at - text) < len; at += wl)
			memset(mark + (at - text), 1, wl);
	}
	/* Runs of plain and highlighted text. */
	for (i = 0; i < len && cols < w; i = j) {
		for (j = i; j < len && mark[j] == mark[i]; j++)
			;
		c = tui_text(t, y, x + cols, w - cols, mark[i] ?
		    (int)(COLOR_PAIR(C_WARN) | A_BOLD | A_UNDERLINE) : attr,
		    "%.*s", (int)(j - i), text + i);
		if (c == 0)
			break;
		cols += c;
	}
	return cols;
}

static void
draw_detail(struct tui *t, int y, int x, int w, const struct package *p)
{
	char size[16], day[16] = "unknown", line[160];
	struct tm tm;
	int c;

	if (p->installed > 0) {
		localtime_r(&p->installed, &tm);
		strftime(day, sizeof(day), "%Y-%m-%d", &tm);
	}
	c = tui_text(t, y, x, w, COLOR_PAIR(C_TITLE) | A_BOLD, "%s %s",
	    p->name, p->version);
	snprintf(line, sizeof(line), "  %s %s installed  %s installed on %s",
	    t->sym->sep, p->size > 0 ? fmt_size(p->size, size, sizeof(size)) :
	    "?", t->sym->sep, day);
	tui_text(t, y, x + c, w - c, COLOR_PAIR(C_DIM), "%s", line);
	marked(t, y + 1, x, w, 0, p->comment);
}

static void
draw(struct tui *t, int y, int x, int h, int w)
{
	char right[64], name[160];
	const struct package *p;
	int rows, r, c, iw = w - 4, left = x + 2, nc;
	size_t i;

	if (!pg.loaded)
		load();
	if (pg.ps.manager != NULL)
		snprintf(right, sizeof(right), "%zu installed (%s)", pg.ps.n,
		    pg.ps.manager);
	else
		snprintf(right, sizeof(right), "no package manager found");
	tui_box(t, y, x, h, w, "Packages", right);

	/* The search field always has the focus. */
	c = tui_text(t, y + 1, left, iw, COLOR_PAIR(C_ACCENT) | A_BOLD,
	    "Search ");
	c += tui_text(t, y + 1, left + c, iw - c, A_BOLD, "%s", pg.query);
	tui_text(t, y + 1, left + c, 1, A_REVERSE, " ");
	if (pg.qlen == 0)
		tui_text(t, y + 1, left + c + 2, iw - c - 2, COLOR_PAIR(C_DIM),
		    "type to search names and descriptions");
	tui_text(t, y + 2, left, iw, COLOR_PAIR(C_DIM), pg.nwords > 0 ?
	    "%zu matching, best first" : "%zu packages", pg.nhits);

	/* The list, scrolled to keep the selection visible. */
	rows = h - 4 - (DETAIL_ROWS + 1);
	if (rows < 1)
		return;
	if (pg.sel < pg.top)
		pg.top = pg.sel;
	if (pg.sel >= pg.top + (size_t)rows)
		pg.top = pg.sel - (size_t)rows + 1;
	nc = iw >= 100 ? NAME_COLS : iw / 3;
	for (r = 0; r < rows && pg.top + (size_t)r < pg.nhits; r++) {
		i = pg.top + (size_t)r;
		p = &pg.ps.p[pg.hits[i]];
		if (i == pg.sel) {
			attron(COLOR_PAIR(C_SEL));
			mvhline(y + 3 + r, left - 1, ' ', iw + 2);
			attroff(COLOR_PAIR(C_SEL));
		}
		c = marked(t, y + 3 + r, left, nc - 1, i == pg.sel ?
		    (int)(COLOR_PAIR(C_SEL) | A_BOLD) : 0, p->name);
		snprintf(name, sizeof(name), "-%s", p->version);
		tui_text(t, y + 3 + r, left + c, nc - 1 - c, i == pg.sel ?
		    (int)COLOR_PAIR(C_SEL) : (int)COLOR_PAIR(C_DIM), "%s", name);
		marked(t, y + 3 + r, left + nc, iw - nc, i == pg.sel ?
		    (int)COLOR_PAIR(C_SEL) : 0, p->comment);
	}

	/* The selected package. */
	r = y + h - 1 - DETAIL_ROWS;
	for (c = 1; c < w - 1; c++)
		tui_text(t, r, x + c, 1, COLOR_PAIR(C_DIM), "%s", t->sym->h);
	if (pg.nhits > 0)
		draw_detail(t, r + 1, left, iw, &pg.ps.p[pg.hits[pg.sel]]);
	else if (pg.ps.manager != NULL)
		tui_text(t, r + 1, left, iw, COLOR_PAIR(C_DIM), "No package "
		    "matches every word.");
}

static int
key(struct tui *t, int ch)
{
	size_t page = (size_t)(LINES > 20 ? LINES - 14 : 6);

	(void)t;
	if (!pg.loaded)		/* a key may come before the first draw */
		load();
	switch (ch) {
	case KEY_DOWN:
		if (pg.sel + 1 < pg.nhits)
			pg.sel++;
		return 1;
	case KEY_UP:
		if (pg.sel > 0)
			pg.sel--;
		return 1;
	case KEY_NPAGE:
		pg.sel = pg.sel + page < pg.nhits ? pg.sel + page :
		    pg.nhits > 0 ? pg.nhits - 1 : 0;
		return 1;
	case KEY_PPAGE:
		pg.sel = pg.sel > page ? pg.sel - page : 0;
		return 1;
	case KEY_HOME:
		pg.sel = 0;
		return 1;
	case KEY_END:
		pg.sel = pg.nhits > 0 ? pg.nhits - 1 : 0;
		return 1;
	case KEY_BACKSPACE:
	case 127:
	case '\b':
		if (pg.qlen == 0)
			return 0;
		pg.query[--pg.qlen] = '\0';
		break;
	case 27:	/* Esc */
	case 'U' - '@':	/* ^U */
		pg.qlen = 0;
		pg.query[0] = '\0';
		break;
	default:
		/* Typing goes to the search field. */
		if (ch < ' ' || ch > '~' || pg.qlen + 1 >= QUERY_MAX)
			return 0;
		pg.query[pg.qlen++] = (char)ch;
		pg.query[pg.qlen] = '\0';
		break;
	}
	if (pg.loaded && pg.hits != NULL)
		search();
	return 1;
}

static void
fini(struct tui *t)
{
	(void)t;
	pkgs_free(&pg.ps);
	free(pg.hits);
	free(pg.score);
	memset(&pg, 0, sizeof(pg));
}

const struct page page_packages = {
	"Packages",
	"type to search  arrows select  Esc clear",
	draw,
	key,
	NULL,
	1,
	fini,
};
