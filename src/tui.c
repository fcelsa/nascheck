/* tui.c - the ncurses interface: main loop, header, footer and helpers. */

#include <sys/stat.h>

#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include "daemon.h"
#include "tui.h"
#include "wcurses.h"

#define TICK_MS		1000	/* getch() timeout: the clock needs no more */
#define WATCH_SECONDS	5	/* how often to look for new data */
#define MIN_COLS	80
#define MIN_LINES	24

static const struct symbols utf8_sym = {
	"─", "│", "┌", "┐", "└", "┘", "┤",
	"●", "▲", "✖", "▶", "·", "›", "■", "·", "°"
};
static const struct symbols ascii_sym = {
	"-", "|", "+", "+", "+", "+", "+",
	"*", "!", "X", ">", "-", ">", "#", ".", ""
};

const struct period tui_periods[] = {
	{ "1h", 3600 }, { "24h", 86400 }, { "7d", 7 * 86400 },
	{ "30d", 30 * 86400 },
};
const int tui_nperiods = sizeof(tui_periods) / sizeof(tui_periods[0]);

/* The pages, in tab order: F1, F2, ... (and 1, 2, ...) select them. */
static const struct page *const pages[] = {
	&page_disks, &page_system, &page_packages, &page_health,
};
#define NPAGES	((int)(sizeof(pages) / sizeof(pages[0])))

/* Columns taken by the first n bytes of s, stopping at width w. */
static size_t
fit_bytes(const char *s, int w, int *cols)
{
	mbstate_t ps;
	size_t i = 0, len = strlen(s), n;
	wchar_t wc;
	int cw;

	memset(&ps, 0, sizeof(ps));
	*cols = 0;
	while (i < len) {
		n = mbrtowc(&wc, s + i, len - i, &ps);
		if (n == (size_t)-1 || n == (size_t)-2 || n == 0)
			break;
		if ((cw = wcwidth(wc)) < 0)
			cw = 1;
		if (*cols + cw > w)
			break;
		*cols += cw;
		i += n;
	}
	return i;
}

int
tui_text(struct tui *t, int y, int x, int w, int attr, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	size_t n;
	int cols;

	(void)t;
	if (w <= 0)
		return 0;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	n = fit_bytes(buf, w, &cols);
	attron(attr);
	mvaddnstr(y, x, buf, (int)n);
	attroff(attr);
	return cols;
}

/* A run of n copies of a one-column symbol. */
static void
rule(int y, int x, int n, const char *s, int attr)
{
	attron(attr);
	for (move(y, x); n > 0; n--)
		addstr(s);
	attroff(attr);
}

void
tui_box(struct tui *t, int y, int x, int h, int w, const char *title,
    const char *right)
{
	const struct symbols *s = t->sym;
	int dim = COLOR_PAIR(C_DIM), i, cols;

	if (h < 2 || w < 4)
		return;
	attron(dim);
	mvaddstr(y, x, s->tl);
	mvaddstr(y, x + w - 1, s->tr);
	mvaddstr(y + h - 1, x, s->bl);
	mvaddstr(y + h - 1, x + w - 1, s->br);
	for (i = 1; i < h - 1; i++) {
		mvaddstr(y + i, x, s->v);
		mvaddstr(y + i, x + w - 1, s->v);
	}
	attroff(dim);
	rule(y, x + 1, w - 2, s->h, dim);
	rule(y + h - 1, x + 1, w - 2, s->h, dim);
	if (title != NULL)
		tui_text(t, y, x + 2, w - 4, COLOR_PAIR(C_TITLE) | A_BOLD,
		    " %s ", title);
	if (right != NULL) {
		fit_bytes(right, w, &cols);
		if (cols + 4 < w)
			tui_text(t, y, x + w - cols - 3, cols + 2, 0, " %s ",
			    right);
	}
}

int
tui_level_attr(enum level l)
{
	return COLOR_PAIR(l == LEVEL_CRIT ? C_CRIT : l == LEVEL_WARN ?
	    C_WARN : C_OK);
}

/* Cold to hot: below warn a gradient, then the warning and critical hues. */
static int
heat(double v, double lo, double warn, double crit)
{
	double top = warn > lo ? warn : lo + 1;
	int i;

	if (crit > 0 && v >= crit)
		return C_HEAT5;
	if (warn > 0 && v >= warn)
		return C_HEAT4;
	i = (int)((v - lo) / (top - lo) * 4);
	return C_HEAT0 + (i < 0 ? 0 : i > 3 ? 3 : i);
}

void
tui_graph(struct tui *t, int y, int x, int h, int w, const struct series *s,
    time_t t0, time_t t1, double lo, double hi, double warn, double crit,
    time_t max_gap)
{
	/* Braille cells hold 2x4 dots; plain terminals get one per cell. */
	static const int left[4] = { 6, 2, 1, 0 }, right[4] = { 7, 5, 4, 3 };
	int sub = t->utf8 ? 2 : 1, dots = t->utf8 ? 4 : 1;
	int cols = w * sub, levels = h * dots, c, r, k, last, lv, bits, base;
	char line[4 * 512];
	unsigned cp;
	size_t i, n;
	int *col;

	if (w <= 0 || h <= 0 || cols > 1024 || t1 <= t0 || hi <= lo ||
	    (col = calloc((size_t)cols, sizeof(*col))) == NULL)
		return;
	/* The highest sample in each column. */
	for (i = 0; i < s->n; i++) {
		c = (int)((s->t[i] - t0) * cols / (t1 - t0));
		if (c < 0 || c >= cols)
			continue;
		lv = (int)((s->v[i] - lo) / (hi - lo) * levels + 0.5);
		lv = lv < 1 ? 1 : lv > levels ? levels : lv;
		if (lv > col[c])
			col[c] = lv;
	}
	/* Bridge short gaps between samples, so that a series reads as one. */
	for (last = -1, c = 0; c < cols; c++) {
		if (col[c] > 0) {
			if (last >= 0 && (time_t)(c - last) * (t1 - t0) / cols <=
			    max_gap)
				for (k = last + 1; k < c; k++)
					col[k] = col[last];
			last = c;
		}
	}
	for (r = 0; r < h; r++) {
		base = (h - 1 - r) * dots;
		for (n = 0, c = 0; c < w; c++) {
			if (!t->utf8) {
				line[n++] = col[c] > base ? '#' : ' ';
				continue;
			}
			for (bits = 0, k = 0; k < 4; k++) {
				if (col[2 * c] > base + k)
					bits |= 1 << left[k];
				if (col[2 * c + 1] > base + k)
					bits |= 1 << right[k];
			}
			if (bits == 0) {
				line[n++] = ' ';
				continue;
			}
			cp = 0x2800u + (unsigned)bits;	/* UTF-8, 3 bytes */
			line[n++] = (char)(0xe0 | (cp >> 12));
			line[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
			line[n++] = (char)(0x80 | (cp & 0x3f));
		}
		line[n] = '\0';
		attron(COLOR_PAIR(heat(lo + (base + dots / 2.0) / levels *
		    (hi - lo), lo, warn, crit)));
		mvaddstr(y + r, x, line);
		attroff(A_COLOR);
	}
	free(col);
}

static void
init_colors(void)
{
	/* 256-color palette, and the basic colors as a fallback. */
	static const struct {
		short	c256;
		short	c8;
	} fg[] = {
		[C_DIM] = { 242, COLOR_BLUE },
		[C_TITLE] = { 255, COLOR_WHITE },
		[C_OK] = { 114, COLOR_GREEN },
		[C_WARN] = { 221, COLOR_YELLOW },
		[C_CRIT] = { 203, COLOR_RED },
		[C_ACCENT] = { 75, COLOR_CYAN },
		[C_SEL] = { 255, COLOR_BLACK },
		[C_HEAT0] = { 39, COLOR_BLUE },
		[C_HEAT1] = { 44, COLOR_CYAN },
		[C_HEAT2] = { 78, COLOR_GREEN },
		[C_HEAT3] = { 186, COLOR_GREEN },
		[C_HEAT4] = { 214, COLOR_YELLOW },
		[C_HEAT5] = { 196, COLOR_RED },
	};
	int i, rich;

	if (!has_colors())
		return;
	start_color();
	use_default_colors();
	rich = COLORS >= 256;
	for (i = C_DIM; i <= C_HEAT5; i++)
		init_pair((short)i, rich ? fg[i].c256 : fg[i].c8,
		    i == C_SEL ? (rich ? 237 : COLOR_WHITE) : -1);
}

/* Tabs on the left; host, overall state, data age and clock on the right. */
static void
header(struct tui *t)
{
	char right[192], clock[8] = "", age[48] = "";
	const struct set *s = &t->set;
	enum level worst = LEVEL_OK;
	struct tm tm;
	time_t now = time(NULL);
	size_t i, warns = 0;
	int x, cols;
	long silence;

	x = tui_text(t, 0, 0, COLS, COLOR_PAIR(C_ACCENT) | A_BOLD,
	    " nascheck ");
	for (i = 0; i < (size_t)NPAGES; i++)
		x += tui_text(t, 0, x, COLS - x, (int)i == t->page ?
		    COLOR_PAIR(C_SEL) | A_BOLD : COLOR_PAIR(C_DIM),
		    " %zu %s ", i + 1, pages[i]->name);

	for (i = 0; i < s->n + s->zfs.n; i++) {
		enum level l = i < s->n ? s->dh[i].level :
		    s->ph[i - s->n].level;
		warns += l != LEVEL_OK;
		if (l > worst)
			worst = l;
	}
	if (s->from_db) {
		silence = daemon_silence(t->cfg, s, now);
		if (silence >= 0)
			snprintf(age, sizeof(age), "daemon silent for %ld min",
			    silence);
		else
			snprintf(age, sizeof(age), "daemon %lld min ago",
			    (long long)(now - s->heartbeat) / 60);
	} else {
		snprintf(age, sizeof(age), "live read");
	}
	if (!t->snapshot) {
		localtime_r(&now, &tm);
		strftime(clock, sizeof(clock), "%H:%M", &tm);
	}
	/* The full summary, or a shorter one on a narrow screen. */
	snprintf(right, sizeof(right), "%s %s %zu disks %s %zu pools %s "
	    "%zu with findings   %s  %s ", level_name(worst), t->sym->sep, s->n,
	    t->sym->sep, s->zfs.n, t->sym->sep, warns, age, clock);
	fit_bytes(right, COLS, &cols);
	if (x + cols >= COLS) {
		snprintf(right, sizeof(right), "%s  %s  %s ", level_name(worst),
		    age, clock);
		fit_bytes(right, COLS, &cols);
	}
	if (x + cols < COLS) {
		x = COLS - cols;
		x += tui_text(t, 0, x, COLS - x, tui_level_attr(worst) |
		    A_BOLD, "%s", level_name(worst));
		tui_text(t, 0, x, COLS - x, 0, "%s", right +
		    strlen(level_name(worst)));
	}
}

static void
footer(struct tui *t)
{
	char keys[48];
	int x, y = LINES - 1, cols;

	x = tui_text(t, y, 0, COLS, COLOR_PAIR(C_DIM), " %s",
	    pages[t->page]->help);
	if (t->msg[0] != '\0')
		x += 3 + tui_text(t, y, x + 3, COLS - x - 3,
		    COLOR_PAIR(C_ACCENT), "%s", t->msg);
	/* The global keys on the right; a page that takes text keeps q. */
	snprintf(keys, sizeof(keys), "F1-F%d/Tab pages  %s quit ", NPAGES,
	    pages[t->page]->text_input ? "F10" : "q");
	cols = (int)strlen(keys);
	if (x + 3 + cols <= COLS)
		tui_text(t, y, COLS - cols, cols, COLOR_PAIR(C_DIM), "%s", keys);
}

static void
draw(struct tui *t)
{
	erase();
	if (COLS < MIN_COLS || LINES < MIN_LINES) {
		tui_text(t, 0, 0, COLS, 0, "nascheck needs at least %dx%d "
		    "characters", MIN_COLS, MIN_LINES);
	} else {
		header(t);
		pages[t->page]->draw(t, 1, 0, LINES - 2, COLS);
		footer(t);
	}
	refresh();
}

/* Read everything again; the pages drop what they derived from it. */
static void
load(struct tui *t)
{
	int i;

	set_free(&t->set);
	if (get_set(t->cfg, t->b, NULL, 0, t->live, &t->set) != 0 ||
	    evaluate(t->cfg, &t->set, time(NULL)) != 0)
		snprintf(t->msg, sizeof(t->msg), "%s", t->set.problem != NULL ?
		    t->set.problem : "no data available");
	for (i = 0; i < NPAGES; i++)
		if (pages[i]->reload != NULL)
			pages[i]->reload(t);
}

/* A cheap fingerprint of the database: the daemon writes to its WAL. */
static time_t
db_stamp(const struct config *c)
{
	char wal[CONFIG_PATH_MAX + 8];
	struct stat st;
	time_t stamp = 0;

	if (stat(c->database, &st) == 0)
		stamp = st.st_mtime;
	snprintf(wal, sizeof(wal), "%s-wal", c->database);
	if (stat(wal, &st) == 0 && st.st_mtime > stamp)
		stamp = st.st_mtime;
	return stamp;
}

/* "r" on page 1: ask the daemon to read the disks, or read them ourselves. */
void
tui_refresh_disks(struct tui *t)
{
	if (t->set.from_db) {
		snprintf(t->msg, sizeof(t->msg), "reading the disks...");
		draw(t);
		snprintf(t->msg, sizeof(t->msg), "%s",
		    daemon_refresh(t->cfg) == 0 ? "disks read" :
		    "daemon unreachable");
	}
	load(t);
}

/*
 * A key for the interface itself; 0 if it is the page's. Pages: F1..F4,
 * Alt+1..Alt+4, Tab and Shift-Tab. Letters stay free on a page that takes
 * text (q, r).
 */
static int
global_key(struct tui *t, int ch)
{
	if ((ch >= KEY_F(1) && ch <= KEY_F(4)) ||
	    (ch >= KEY_ALT(1) && ch <= KEY_ALT(4))) {
		ch = ch >= KEY_ALT(1) ? ch - KEY_ALT(1) : ch - KEY_F(1);
		if (ch < NPAGES)
			t->page = ch;
		return 1;
	}
	if (ch == '\t' || ch == KEY_BTAB) {
		t->page = (t->page + (ch == '\t' ? 1 : NPAGES - 1)) % NPAGES;
		return 1;
	}
	if ((ch == 'r' || ch == 'R') && !pages[t->page]->text_input &&
	    pages[t->page]->reread != NULL) {
		pages[t->page]->reread(t);
		return 1;
	}
	return ch == KEY_RESIZE;
}

static int
quit_key(const struct tui *t, int ch)
{
	return ch == KEY_F(10) || (!pages[t->page]->text_input &&
	    (ch == 'q' || ch == 'Q'));
}

/* Esc followed at once by a digit is Alt+digit; Esc alone stays Esc. */
static int
read_key(void)
{
	int ch = getch(), next;

	if (ch != 27)
		return ch;
	timeout(0);
	next = getch();
	timeout(TICK_MS);
	if (next >= '1' && next <= '9')
		return KEY_ALT(next - '0');
	if (next != ERR)
		ungetch(next);
	return ch;
}

/*
 * Testing: keys as text. "@" escapes the others: @1..@9 F1..F9, @a1..@a9
 * Alt+1..Alt+9, @t Tab, @d @u down and up, @n @p page down and up,
 * @b backspace, @e Esc.
 */
static int
snapshot_key(const char **keys)
{
	int ch = (unsigned char)*(*keys)++;

	if (ch != '@' || **keys == '\0')
		return ch;
	ch = (unsigned char)*(*keys)++;
	if (ch >= '1' && ch <= '9')
		return KEY_F(ch - '0');
	switch (ch) {
	case 'a':
		if (**keys >= '1' && **keys <= '9')
			return KEY_ALT(*(*keys)++ - '0');
		return ch;
	case 't':
		return '\t';
	case 'd':
		return KEY_DOWN;
	case 'u':
		return KEY_UP;
	case 'n':
		return KEY_NPAGE;
	case 'p':
		return KEY_PPAGE;
	case 'b':
		return KEY_BACKSPACE;
	case 'e':
		return 27;
	}
	return ch;
}

/* Testing: print the screen as text, one line per row. */
static void
dump_screen(void)
{
	char out[MB_LEN_MAX * 4 * 512], mb[MB_LEN_MAX];
	wchar_t wch[CCHARW_MAX + 1];
	mbstate_t ps;
	cchar_t cc;
	attr_t a;
	short pair;
	size_t n, k, len;
	int y, x, i;

	for (y = 0; y < LINES; y++) {
		memset(&ps, 0, sizeof(ps));
		for (n = 0, x = 0; x < COLS && x < 512; x++) {
			if (mvin_wch(y, x, &cc) == ERR ||
			    getcchar(&cc, wch, &a, &pair, NULL) == ERR)
				continue;
			for (i = 0; wch[i] != L'\0' && i < CCHARW_MAX; i++) {
				if ((len = wcrtomb(mb, wch[i], &ps)) ==
				    (size_t)-1)
					continue;
				for (k = 0; k < len && n < sizeof(out) - 1; k++)
					out[n++] = mb[k];
			}
		}
		while (n > 0 && out[n - 1] == ' ')
			n--;
		out[n] = '\0';
		puts(out);
	}
}

static int
snapshot(struct tui *t, const char *keys)
{
	const char *term = getenv("TERM");
	SCREEN *scr;
	FILE *null;
	int ch;

	if ((null = fopen("/dev/null", "w")) == NULL ||
	    (scr = newterm(term != NULL ? term : "xterm-256color", null,
	    stdin)) == NULL)
		return 1;
	set_term(scr);
	init_colors();
	/* Draw after every key, as the interactive loop does: pages learn
	 * their geometry while drawing. */
	draw(t);
	while (*keys != '\0') {
		ch = snapshot_key(&keys);
		if (!global_key(t, ch))
			pages[t->page]->key(t, ch);
		draw(t);
	}
	dump_screen();
	endwin();
	delscreen(scr);
	fclose(null);
	return 0;
}

int
tui_run(const struct config *cfg, int live)
{
	struct tui t;
	const char *keys = getenv("NASCHECK_TUI_SNAPSHOT");
	struct buf b = { 0 };
	time_t stamp, now, minute = 0, watched = 0;
	int ch, dirty = 1, rc = 0, saved_err = -1, null, i;

	memset(&t, 0, sizeof(t));
	t.cfg = cfg;
	t.b = &b;
	t.live = live;
	t.period = 1;		/* 24h */
	setlocale(LC_ALL, "");
#if defined(NASCHECK_STATIC) && defined(__linux__)
	/* A static binary knows the terminfo directory of the system it was
	 * built on only: give it those of the usual distributions. */
	setenv("TERMINFO_DIRS", "/etc/terminfo:/lib/terminfo:"
	    "/usr/share/terminfo:/usr/lib/terminfo", 0);
#endif
	t.utf8 = strcmp(nl_langinfo(CODESET), "UTF-8") == 0;
	/* Recording a demo: no user, host, address or disk labels shown. */
	t.demo = getenv("NASCHECK_DEMO") != NULL;
	t.sym = t.utf8 ? &utf8_sym : &ascii_sym;
	load(&t);		/* errors still reach the terminal */
	stamp = db_stamp(cfg);

	if (keys != NULL) {
		t.snapshot = 1;
		rc = snapshot(&t, keys);
		goto out;
	}
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fprintf(stderr, "nascheck: the interface needs a terminal; "
		    "see nascheck -h\n");
		rc = 1;
		goto out;
	}
	/* Messages on stderr would scribble over the screen. */
	if ((null = open("/dev/null", O_WRONLY)) != -1) {
		saved_err = dup(STDERR_FILENO);
		dup2(null, STDERR_FILENO);
		close(null);
	}
	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	set_escdelay(25);	/* Esc alone, without waiting a second */
	curs_set(0);
	timeout(TICK_MS);
	init_colors();

	for (;;) {
		if (dirty) {
			draw(&t);
			dirty = 0;
		}
		ch = read_key();
		now = time(NULL);
		if (ch == ERR) {
			/* Idle: redraw for the clock, reload on new data. */
			if (now / 60 != minute) {
				minute = now / 60;
				dirty = 1;
			}
			if (now - watched >= WATCH_SECONDS) {
				watched = now;
				if (t.set.from_db && db_stamp(cfg) != stamp) {
					stamp = db_stamp(cfg);
					load(&t);
					dirty = 1;
				}
			}
			continue;
		}
		if (quit_key(&t, ch))
			break;
		t.msg[0] = '\0';
		if (global_key(&t, ch) || pages[t.page]->key(&t, ch))
			dirty = 1;
	}
	endwin();
	if (saved_err != -1) {
		dup2(saved_err, STDERR_FILENO);
		close(saved_err);
	}
out:
	for (i = 0; i < NPAGES; i++)
		if (pages[i]->fini != NULL)
			pages[i]->fini(&t);
	set_free(&t.set);
	series_free(&t.hist);
	free(t.ds);
	buf_free(&t.dsbuf);
	buf_free(&b);
	return rc;
}
