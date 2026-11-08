/* pkgs.h - the packages installed on the host, from its package manager. */
#ifndef NASCHECK_PKGS_H
#define NASCHECK_PKGS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "run.h"

struct package {
	const char	*name;
	const char	*version;
	const char	*comment;	/* one-line description */
	uint64_t	 size;		/* installed bytes, 0 if unknown */
	time_t		 installed;	/* 0 if unknown */
};

struct packages {
	struct buf	 b;		/* the manager's output, split in place */
	struct package	*p;		/* sorted by name, pointing into b */
	size_t		 n;
	const char	*manager;	/* "pkg", "dpkg", "rpm" */
};

/*
 * Ask the first package manager found: pkg (FreeBSD), dpkg, rpm.
 * Returns 0, or -1 when there is none.
 */
int	pkgs_load(struct packages *ps);

void	pkgs_free(struct packages *ps);

#endif
