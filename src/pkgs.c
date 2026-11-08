/* pkgs.c - the packages installed on the host, from its package manager. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "pkgs.h"

/*
 * Every manager prints the same five tab-separated fields per package:
 * name, version, size, install time, description.
 */
static const struct manager {
	const char	*name;
	char		*argv[6];
	uint64_t	 size_unit;	/* bytes per size unit */
} managers[] = {
	{ "pkg", { "pkg", "query", "%n\t%v\t%sb\t%t\t%c", NULL }, 1 },
	{ "dpkg", { "dpkg-query", "-W", "-f", "${Package}\t${Version}\t"
	    "${Installed-Size}\t0\t${binary:Summary}\n", NULL }, 1024 },
	{ "rpm", { "rpm", "-qa", "--qf", "%{NAME}\t%{VERSION}-%{RELEASE}\t"
	    "%{SIZE}\t%{INSTALLTIME}\t%{SUMMARY}\n", NULL }, 1 },
};

static int
by_name(const void *a, const void *b)
{
	return strcmp(((const struct package *)a)->name,
	    ((const struct package *)b)->name);
}

/* Split the output in place into packages. */
static int
parse(struct packages *ps, uint64_t unit)
{
	char *line, *next, *f[5];
	struct package *pk;
	size_t cap = 0;
	void *p;
	int k;

	for (line = ps->b.data; line != NULL && *line != '\0'; line = next) {
		if ((next = strchr(line, '\n')) != NULL)
			*next++ = '\0';
		for (k = 0; k < 5 && line != NULL; k++) {
			f[k] = line;
			if (k < 4 && (line = strchr(line, '\t')) != NULL)
				*line++ = '\0';
		}
		if (k < 5 || f[0][0] == '\0')
			continue;
		if (ps->n == cap) {
			cap = cap ? cap * 2 : 512;
			if ((p = realloc(ps->p, cap * sizeof(*ps->p))) == NULL)
				return -1;
			ps->p = p;
		}
		pk = &ps->p[ps->n++];
		pk->name = f[0];
		pk->version = f[1];
		pk->size = strtoull(f[2], NULL, 10) * unit;
		pk->installed = (time_t)strtoll(f[3], NULL, 10);
		pk->comment = f[4];
	}
	qsort(ps->p, ps->n, sizeof(*ps->p), by_name);
	return 0;
}

int
pkgs_load(struct packages *ps)
{
	const char *fake = getenv("NASCHECK_PKG");	/* tests: a stand-in */
	char *argv[6];
	size_t i;
	int code;

	memset(ps, 0, sizeof(*ps));
	for (i = 0; i < sizeof(managers) / sizeof(managers[0]); i++) {
		memcpy(argv, managers[i].argv, sizeof(argv));
		if (fake != NULL && *fake != '\0') {
			if (i > 0)
				break;	/* the fake speaks pkg */
			argv[0] = (char *)fake;
		}
		if (run_capture(argv, &ps->b, 1, &code) == -1 || code != 0)
			continue;
		ps->manager = managers[i].name;
		return parse(ps, managers[i].size_unit);
	}
	errno = ENOENT;
	return -1;
}

void
pkgs_free(struct packages *ps)
{
	buf_free(&ps->b);
	free(ps->p);
	memset(ps, 0, sizeof(*ps));
}
