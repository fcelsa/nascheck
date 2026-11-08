/* tui.h - the ncurses interface: shared state, pages and drawing helpers. */
#ifndef NASCHECK_TUI_H
#define NASCHECK_TUI_H

#include <time.h>

#include "config.h"
#include "data.h"
#include "db.h"

struct tui;

/*
 * A page fills the screen between the header and the footer. Adding one
 * means writing these functions and listing it in tui.c.
 */
struct page {
	const char	*name;		/* tab label */
	const char	*help;		/* its keys, for the footer */
	void		(*draw)(struct tui *t, int y, int x, int h, int w);
	int		(*key)(struct tui *t, int ch);	/* 1 if handled */
	void		(*reload)(struct tui *t);	/* new data, or NULL */
	void		(*reread)(struct tui *t);	/* "r", or NULL */
	int		text_input;	/* printable keys go to the page */
	void		(*fini)(struct tui *t);	/* free its state, or NULL */
};

/* Alt+1..Alt+9 (Esc then the digit), past the keys curses knows. */
#define KEY_ALT(n)	(KEY_MAX + 1 + (n))

/* Read the disks again: through the daemon, or directly. */
void	tui_refresh_disks(struct tui *t);

/* Color pairs. */
enum {
	C_DIM = 1, C_TITLE, C_OK, C_WARN, C_CRIT, C_ACCENT, C_SEL,
	C_HEAT0, C_HEAT1, C_HEAT2, C_HEAT3, C_HEAT4, C_HEAT5	/* cold..hot */
};

/* Symbols in UTF-8, with ASCII stand-ins for other terminals. */
struct symbols {
	const char *h, *v, *tl, *tr, *bl, *br, *tee;	/* box drawing */
	const char *dot, *warn, *crit, *sel, *sep, *arrow, *full, *empty;
	const char *deg;
};

struct tui {
	const struct config	*cfg;
	struct buf		*b;
	int			 live;		/* read the disks, not the db */
	int			 utf8;
	const struct symbols	*sym;
	struct set		 set;		/* what the pages show */
	char			 msg[96];	/* footer message */
	int			 page;
	int			 snapshot;	/* testing: no clock */

	/* Disks page. */
	size_t			 sel;		/* disks first, then pools */
	size_t			 top;		/* first listed row */
	int			 period;	/* index into tui_periods */
	int			 hist_valid;
	struct series		 hist;		/* graph of the selection */
	int			 base_ids[ATA_MAX_ATTRS];
	int64_t			 base_raw[ATA_MAX_ATTRS];
	int			 nbase;
	time_t			 base_time;
	int			 detail_off;	/* details scrolled by rows */
	int			 detail_n;	/* rows of the last draw */
	int			 detail_rows;	/* rows of the panel */
	struct buf		 dsbuf;		/* datasets of the pool */
	struct dataset		*ds;
	int			 nds;
};

struct period {
	const char	*label;
	time_t		 seconds;
};

extern const struct period tui_periods[];
extern const int tui_nperiods;
extern const struct page page_disks;
extern const struct page page_system;
extern const struct page page_packages;

/*
 * Run the interface until the user quits. With NASCHECK_TUI_SNAPSHOT set,
 * feed its value as keys, print the screen as text and exit (tests).
 * Returns the process exit code.
 */
int	tui_run(const struct config *cfg, int live);

/* Drawing helpers for pages. */
void	tui_box(struct tui *t, int y, int x, int h, int w, const char *title,
	    const char *right);
/* Text clipped to w columns; returns the columns written. */
int	tui_text(struct tui *t, int y, int x, int w, int attr,
	    const char *fmt, ...);
int	tui_level_attr(enum level l);
/*
 * Area graph of s between t0 and t1, values lo..hi, colored from cold to
 * hot against warn/crit. Samples further apart than max_gap leave a gap.
 */
void	tui_graph(struct tui *t, int y, int x, int h, int w,
	    const struct series *s, time_t t0, time_t t1, double lo, double hi,
	    double warn, double crit, time_t max_gap);

#endif
