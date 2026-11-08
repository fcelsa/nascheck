/* pkgs.h - the packages installed on the host: from its package manager,
 * then flatpak and snap. */
#ifndef NASCHECK_PKGS_H
#define NASCHECK_PKGS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "run.h"

/* Why a package is there. */
enum pkg_kind {
	PKG_UNKNOWN,
	PKG_MANUAL,		/* asked for by the administrator */
	PKG_DEPENDENCY,		/* pulled in by another package */
	PKG_SYSTEM,		/* part of the base system */
	PKG_KINDS
};

struct package {
	const char	*name;
	const char	*version;
	const char	*comment;	/* one-line description */
	const char	*repo;		/* repository it came from, "" if unknown */
	const char	*source;	/* "flatpak", "snap"; NULL: the manager */
	uint64_t	 size;		/* installed bytes, 0 if unknown */
	time_t		 installed;	/* 0 if unknown */
	int		 rdeps;		/* packages that need it, -1 if unknown */
	int		 kind;		/* enum pkg_kind */
};

/* The text the packages point into, one buffer per source. */
enum { PB_MANAGER, PB_DNF, PB_FLATPAK, PB_SNAP, PB_SNAPINFO, PB_COUNT };

struct packages {
	struct buf	 b[PB_COUNT];
	struct package	*p;		/* sorted by name */
	size_t		 n, cap;
	size_t		 count[PKG_KINDS];	/* packages of each kind */
	size_t		 nflatpak, nsnap;
	const char	*manager;	/* "pkg", "dpkg", "pacman", "apk", "rpm" */
	const char	*base;		/* how the base system is kept, or NULL */
	char		 repos[512];	/* enabled repositories, ", " between */
};

/*
 * Ask the first package manager found (pkg, dpkg, pacman, apk, rpm), then
 * flatpak and snap. Returns 0, or -1 when there is no package at all.
 */
int	pkgs_load(struct packages *ps);

void	pkgs_free(struct packages *ps);

#endif
