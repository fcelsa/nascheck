/* page_packages.c - page 3: installed packages, with a live search. */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pkgs.h"
#include "tui.h"
#include "wcurses.h"

#define QUERY_MAX	64
#define WORDS_MAX	8
#define NAME_COLS	40	/* name and version column */
#define DETAIL_ROWS	5	/* the selected package, at the bottom */

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
	int		 sort;		/* enum sort */
} pg;

/* "S" cycles the order; by name means best first while searching. */
enum sort { SORT_NAME, SORT_SIZE, SORT_DATE, SORTS };
static const char *const sort_name[SORTS] = { "name", "size", "date" };

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

/* The order of the list: largest or newest first, ties by name. */
static int
by_order(const void *a, const void *b)
{
	const struct package *p = &pg.ps.p[*(const size_t *)a];
	const struct package *q = &pg.ps.p[*(const size_t *)b];
	size_t i = *(const size_t *)a, j = *(const size_t *)b;

	switch (pg.sort) {
	case SORT_SIZE:
		if (p->size != q->size)
			return p->size < q->size ? 1 : -1;
		break;
	case SORT_DATE:
		if (p->installed != q->installed)
			return p->installed < q->installed ? 1 : -1;
		break;
	default:
		if (pg.score[i] != pg.score[j])
			return pg.score[j] - pg.score[i];
	}
	return strcmp(p->name, q->name);
}

/* Sort the hits again, keeping the selected package selected. */
static void
order(void)
{
	size_t was = pg.nhits > 0 ? pg.hits[pg.sel] : 0, i;

	qsort(pg.hits, pg.nhits, sizeof(*pg.hits), by_order);
	for (i = 0; i < pg.nhits && pg.hits[i] != was; i++)
		;
	pg.sel = i < pg.nhits ? i : 0;
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
	/* Without words the hits are in name order already. */
	if (pg.sort != SORT_NAME || pg.nwords > 0)
		qsort(pg.hits, pg.nhits, sizeof(*pg.hits), by_order);
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

/* Add a part to a line, after a separator if it is not the first. */
static void
append(char *line, size_t size, const char *sep, const char *fmt, ...)
{
	size_t n = strlen(line);
	va_list ap;

	if (n > 0 && n < size)
		n += (size_t)snprintf(line + n, size - n, "  %s ", sep);
	if (n < size) {
		va_start(ap, fmt);
		vsnprintf(line + n, size - n, fmt, ap);
		va_end(ap);
	}
}

/* The install day, in the list when it is sorted by date. */
static void
draw_day(struct tui *t, int y, int x, int sel, const struct package *p)
{
	char day[16] = "?";
	struct tm tm;

	if (p->installed > 0) {
		localtime_r(&p->installed, &tm);
		strftime(day, sizeof(day), "%Y-%m-%d", &tm);
	}
	tui_text(t, y, x, 11, sel ? (int)COLOR_PAIR(C_SEL) : 0, "%10s", day);
}

/* A mark for each kind of package, and its color. */
static int
kind_mark(struct tui *t, int y, int x, int kind, int sel)
{
	static const char *const utf8[PKG_KINDS] = { " ", "●", "○", "◆" };
	static const char *const ascii[PKG_KINDS] = { " ", "*", "-", "#" };
	static const short color[PKG_KINDS] = { 0, C_ACCENT, C_DIM, C_TITLE };

	return tui_text(t, y, x, 1, COLOR_PAIR(sel ? C_SEL : color[kind]), "%s",
	    (t->utf8 ? utf8 : ascii)[kind]);
}

static void
draw_detail(struct tui *t, int y, int x, int w, const struct package *p)
{
	static const char *const kind[PKG_KINDS] = { NULL, "installed by hand",
	    "installed as a dependency", "part of the base system" };
	const char *sep = t->sym->sep;
	char size[16], day[16] = "unknown", line[320];
	struct tm tm;
	int c;

	if (p->installed > 0) {
		localtime_r(&p->installed, &tm);
		strftime(day, sizeof(day), "%Y-%m-%d", &tm);
	}
	c = tui_text(t, y, x, w, COLOR_PAIR(C_TITLE) | A_BOLD, "%s %s",
	    p->name, p->version);
	snprintf(line, sizeof(line), "  %s %s installed  %s installed on %s",
	    sep, p->size > 0 ? fmt_size(p->size, size, sizeof(size)) : "?",
	    sep, day);
	tui_text(t, y, x + c, w - c, COLOR_PAIR(C_DIM), "%s", line);
	marked(t, y + 1, x, w, 0, p->comment);

	/* Why it is there, where from, what needs it. */
	line[0] = '\0';
	if (kind[p->kind] != NULL)
		append(line, sizeof(line), sep, "%s", kind[p->kind]);
	if (p->source != NULL)
		append(line, sizeof(line), sep, "%s%s%s", p->source,
		    p->repo[0] != '\0' ? " from " : "", p->repo);
	else if (p->repo[0] != '\0')
		append(line, sizeof(line), sep, "from %s", p->repo);
	if (p->rdeps == 0)
		append(line, sizeof(line), sep, "no package needs it");
	else if (p->rdeps > 0)
		append(line, sizeof(line), sep, "needed by %d package%s",
		    p->rdeps, p->rdeps == 1 ? "" : "s");
	c = kind_mark(t, y + 2, x, p->kind, 0);
	tui_text(t, y + 2, x + c + 1, w - c - 1, 0, "%s", line);

	/* The host: where packages come from, how the base system is kept. */
	c = tui_text(t, y + 3, x, w, COLOR_PAIR(C_ACCENT) | A_BOLD,
	    "Repositories ");
	c += tui_text(t, y + 3, x + c, w - c, COLOR_PAIR(C_DIM), "%s",
	    pg.ps.repos[0] != '\0' ? pg.ps.repos : "none found");
	if (pg.ps.base != NULL)
		tui_text(t, y + 3, x + c, w - c, COLOR_PAIR(C_DIM),
		    "  %s base system via %s", sep, pg.ps.base);
}

static void
draw(struct tui *t, int y, int x, int h, int w)
{
	static const char *const kind[PKG_KINDS] = { NULL, "by hand",
	    "dependencies", "system" };
	char right[160], name[160];
	const struct package *p;
	int rows, r, c, iw = w - 4, left = x + 2, nc, k;
	size_t i, n;

	if (!pg.loaded)
		load();
	if (pg.ps.n > 0) {
		/* "1058 installed (1041 dpkg, 12 flatpak, 5 snap)" */
		n = (size_t)snprintf(right, sizeof(right), "%zu installed (",
		    pg.ps.n);
		if (pg.ps.manager != NULL && pg.ps.nflatpak + pg.ps.nsnap == 0)
			n += (size_t)snprintf(right + n, sizeof(right) - n, "%s",
			    pg.ps.manager);
		else if (pg.ps.manager != NULL)
			n += (size_t)snprintf(right + n, sizeof(right) - n,
			    "%zu %s", pg.ps.n - pg.ps.nflatpak - pg.ps.nsnap,
			    pg.ps.manager);
		if (pg.ps.nflatpak > 0 && n < sizeof(right))
			n += (size_t)snprintf(right + n, sizeof(right) - n,
			    "%s%zu flatpak", pg.ps.manager != NULL ? ", " : "",
			    pg.ps.nflatpak);
		if (pg.ps.nsnap > 0 && n < sizeof(right))
			n += (size_t)snprintf(right + n, sizeof(right) - n,
			    "%s%zu snap", pg.ps.manager != NULL ||
			    pg.ps.nflatpak > 0 ? ", " : "", pg.ps.nsnap);
		if (n < sizeof(right))
			n += (size_t)snprintf(right + n, sizeof(right) - n, ")");
		for (k = PKG_MANUAL; k < PKG_KINDS; k++)
			if (pg.ps.count[k] > 0 && n < sizeof(right))
				n += (size_t)snprintf(right + n, sizeof(right) -
				    n, " %s %zu %s", t->sym->sep, pg.ps.count[k],
				    k == PKG_DEPENDENCY && pg.ps.count[k] == 1 ?
				    "dependency" : kind[k]);
	} else {
		snprintf(right, sizeof(right), "no package manager found");
	}
	tui_box(t, y, x, h, w, "Packages", right);

	/* The search field always has the focus. */
	c = tui_text(t, y + 1, left, iw, COLOR_PAIR(C_ACCENT) | A_BOLD,
	    "Search ");
	c += tui_text(t, y + 1, left + c, iw - c, A_BOLD, "%s", pg.query);
	tui_text(t, y + 1, left + c, 1, A_REVERSE, " ");
	if (pg.qlen == 0)
		tui_text(t, y + 1, left + c + 2, iw - c - 2, COLOR_PAIR(C_DIM),
		    "type to search names and descriptions");
	c = tui_text(t, y + 2, left, iw, COLOR_PAIR(C_DIM), "%zu %s %s "
	    "sorted by %s", pg.nhits, pg.nwords > 0 ? "matching" : "packages",
	    t->sym->sep, pg.sort == SORT_NAME && pg.nwords > 0 ? "relevance" :
	    sort_name[pg.sort]);

	/* What the marks mean, on the right. */
	if (pg.ps.count[PKG_MANUAL] + pg.ps.count[PKG_DEPENDENCY] +
	    pg.ps.count[PKG_SYSTEM] > 0 && iw - c > 44) {
		c = left + iw - 42;
		for (k = PKG_MANUAL; k < PKG_KINDS; k++) {
			c += kind_mark(t, y + 2, c, k, 0);
			c += tui_text(t, y + 2, c, 14, COLOR_PAIR(C_DIM),
			    " %s  ", k == PKG_DEPENDENCY ? "dependency" :
			    k == PKG_MANUAL ? "by hand" : "system");
		}
	}

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
		kind_mark(t, y + 3 + r, left, p->kind, i == pg.sel);
		c = 2 + marked(t, y + 3 + r, left + 2, nc - 3, i == pg.sel ?
		    (int)(COLOR_PAIR(C_SEL) | A_BOLD) : 0, p->name);
		snprintf(name, sizeof(name), "%s%s", p->version[0] != '\0' ?
		    "-" : "", p->version);
		tui_text(t, y + 3 + r, left + c, nc - 1 - c, i == pg.sel ?
		    (int)COLOR_PAIR(C_SEL) : (int)COLOR_PAIR(C_DIM), "%s", name);
		/* The order's key, when it is not the name. */
		if (pg.sort == SORT_SIZE)
			tui_text(t, y + 3 + r, left + nc, 8, i == pg.sel ?
			    (int)COLOR_PAIR(C_SEL) : 0, "%7s", p->size > 0 ?
			    fmt_size(p->size, name, sizeof(name)) : "?");
		else if (pg.sort == SORT_DATE)
			draw_day(t, y + 3 + r, left + nc, i == pg.sel, p);
		k = pg.sort == SORT_NAME ? 0 : pg.sort == SORT_SIZE ? 9 : 12;
		marked(t, y + 3 + r, left + nc + k, iw - nc - k, i == pg.sel ?
		    (int)COLOR_PAIR(C_SEL) : 0, p->comment);
	}

	/* The selected package. */
	r = y + h - 1 - DETAIL_ROWS;
	for (c = 1; c < w - 1; c++)
		tui_text(t, r, x + c, 1, COLOR_PAIR(C_DIM), "%s", t->sym->h);
	if (pg.nhits > 0)
		draw_detail(t, r + 1, left, iw, &pg.ps.p[pg.hits[pg.sel]]);
	else if (pg.ps.n > 0)
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
	case 'S':	/* the search ignores case: capitals are free */
		pg.sort = (pg.sort + 1) % SORTS;
		order();
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
	"type to search  arrows select  S sort  Esc clear",
	draw,
	key,
	NULL,
	1,
	fini,
};
