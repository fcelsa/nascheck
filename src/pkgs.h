/* pkgs.h - the packages installed on the host, from its package manager. */
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
	uint64_t	 size;		/* installed bytes, 0 if unknown */
	time_t		 installed;	/* 0 if unknown */
	int		 rdeps;		/* packages that need it, -1 if unknown */
	int		 kind;		/* enum pkg_kind */
};

struct packages {
	struct buf	 b;		/* the manager's output, split in place */
	struct package	*p;		/* sorted by name, pointing into b */
	size_t		 n;
	size_t		 count[PKG_KINDS];	/* packages of each kind */
	const char	*manager;	/* "pkg", "dpkg", "rpm" */
	const char	*base;		/* how the base system is kept, or NULL */
	char		 repos[512];	/* enabled repositories, ", " between */
};

/*
 * Ask the first package manager found: pkg (FreeBSD), dpkg, rpm.
 * Returns 0, or -1 when there is none.
 */
int	pkgs_load(struct packages *ps);

void	pkgs_free(struct packages *ps);

#endif
