/* wcurses.h - the wide-character curses API, wherever it is installed. */
#ifndef NASCHECK_WCURSES_H
#define NASCHECK_WCURSES_H

#define NCURSES_WIDECHAR 1

/* Some systems (openSUSE) keep it apart from the narrow curses.h. */
#if defined(__has_include)
#if __has_include(<ncursesw/curses.h>)
#include <ncursesw/curses.h>
#else
#include <curses.h>
#endif
#else
#include <curses.h>
#endif

#endif
