/* pkgs.c - the packages installed on the host, from its package manager. */
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pkgs.h"

/*
 * Every manager prints the same tab-separated fields per package, empty
 * where it has nothing to say:
 *
 *   name version size time automatic rdeps repository hint description
 *
 * "hint" is what the manager knows about the kind of package; row() reads
 * it, finish() adds what the manager keeps in files.
 */
enum { F_NAME, F_VERSION, F_SIZE, F_TIME, F_AUTO, F_RDEPS, F_REPO, F_HINT,
    F_COMMENT, NFIELDS };

struct manager {
	const char	*name;
	char		*argv[6];
	uint64_t	 size_unit;	/* bytes per size unit */
	int		(*row)(struct package *pk, char **f);	/* -1: skip */
	void		(*finish)(struct packages *ps);
};

static int	pkg_row(struct package *, char **);
static void	pkg_finish(struct packages *);
static int	dpkg_row(struct package *, char **);
static void	dpkg_finish(struct packages *);
static void	rpm_finish(struct packages *);

static const struct manager managers[] = {
	{ "pkg", { "pkg", "query",
	    "%n\t%v\t%sb\t%t\t%a\t%#r\t%R\t%o\t%c", NULL }, 1,
	    pkg_row, pkg_finish },
	/* The hint: status, priority, essential, architecture. */
	{ "dpkg", { "dpkg-query", "-W", "-f", "${Package}\t${Version}\t"
	    "${Installed-Size}\t\t\t\t\t${db:Status-Abbrev}${Priority} "
	    "${Essential} ${Architecture}\t${binary:Summary}\n", NULL }, 1024,
	    dpkg_row, dpkg_finish },
	{ "rpm", { "rpm", "-qa", "--qf", "%{NAME}\t%{VERSION}-%{RELEASE}\t"
	    "%{SIZE}\t%{INSTALLTIME}\t\t\t\t\t%{SUMMARY}\n", NULL }, 1,
	    NULL, rpm_finish },
};

static const char *pkg_tool = "pkg";	/* or the stand-in of the tests */

static int
by_name(const void *a, const void *b)
{
	return strcmp(((const struct package *)a)->name,
	    ((const struct package *)b)->name);
}

static struct package *
find_name(struct packages *ps, const char *name)
{
	struct package key;

	key.name = name;
	return bsearch(&key, ps->p, ps->n, sizeof(*ps->p), by_name);
}

/* Is word one of the words of list, separated by any of sep? */
static int
has_word(const char *list, const char *word, const char *sep)
{
	size_t n = strlen(word), k;

	for (list += strspn(list, sep); *list != '\0'; list += k,
	    list += strspn(list, sep))
		if ((k = strcspn(list, sep)) == n && strncmp(list, word, n) == 0)
			return 1;
	return 0;
}

/* Append one repository to the list, once. */
static void
add_repo(struct packages *ps, const char *s)
{
	size_t len = strlen(ps->repos), n = strlen(s), k;
	const char *p;

	if (n == 0 || len + n + 3 > sizeof(ps->repos))
		return;
	for (p = ps->repos; *p != '\0'; p += k + (p[k] != '\0' ? 2 : 0))
		if ((k = strcspn(p, ",")) == n && strncmp(p, s, n) == 0)
			return;
	snprintf(ps->repos + len, sizeof(ps->repos) - len, "%s%s",
	    len > 0 ? ", " : "", s);
}

/* Split the output in place into packages. */
static int
parse(struct packages *ps, const struct manager *m)
{
	char *line, *next, *f[NFIELDS];
	struct package *pk;
	size_t cap = 0;
	void *p;
	int k;

	for (line = ps->b.data; line != NULL && *line != '\0'; line = next) {
		if ((next = strchr(line, '\n')) != NULL)
			*next++ = '\0';
		for (k = 0; k < NFIELDS && line != NULL; k++) {
			f[k] = line;
			if (k < NFIELDS - 1 && (line = strchr(line, '\t')) !=
			    NULL)
				*line++ = '\0';
		}
		if (k < NFIELDS || f[F_NAME][0] == '\0')
			continue;
		if (ps->n == cap) {
			cap = cap ? cap * 2 : 512;
			if ((p = realloc(ps->p, cap * sizeof(*ps->p))) == NULL)
				return -1;
			ps->p = p;
		}
		pk = &ps->p[ps->n];
		pk->name = f[F_NAME];
		pk->version = f[F_VERSION];
		pk->comment = f[F_COMMENT];
		pk->repo = f[F_REPO];
		pk->size = strtoull(f[F_SIZE], NULL, 10) * m->size_unit;
		pk->installed = (time_t)strtoll(f[F_TIME], NULL, 10);
		pk->rdeps = f[F_RDEPS][0] != '\0' ? atoi(f[F_RDEPS]) : -1;
		pk->kind = PKG_UNKNOWN;
		if (m->row == NULL || m->row(pk, f) == 0)
			ps->n++;
	}
	qsort(ps->p, ps->n, sizeof(*ps->p), by_name);
	return 0;
}

/*
 * pkg: base system packages (pkgbase) are named FreeBSD-* with an origin
 * under base/; the others are marked automatic or not.
 */
static int
pkg_row(struct package *pk, char **f)
{
	if (strncmp(f[F_HINT], "base/", 5) == 0 ||
	    strncmp(pk->name, "FreeBSD-", 8) == 0)
		pk->kind = PKG_SYSTEM;
	else if (f[F_AUTO][0] != '\0')
		pk->kind = f[F_AUTO][0] == '1' ? PKG_DEPENDENCY : PKG_MANUAL;
	if (strcmp(pk->repo, "unknown-repository") == 0)
		pk->repo = "";		/* pkg add, or built from ports */
	return 0;
}

/* The enabled repositories, from the configuration that pkg -vv prints. */
static void
pkg_finish(struct packages *ps)
{
	char *argv[] = { (char *)pkg_tool, "-vv", NULL }, *line, *next, *s;
	char name[64] = "";
	struct buf b = { 0 };
	size_t i;
	int code, in = 0;


	for (i = 0; i < ps->n && ps->p[i].kind != PKG_SYSTEM; i++)
		;
	ps->base = i < ps->n ? "pkgbase" : "freebsd-update";
	if (run_capture(argv, &b, 1, &code) == -1 || code != 0)
		goto out;
	for (line = b.data; line != NULL && *line != '\0'; line = next) {
		if ((next = strchr(line, '\n')) != NULL)
			*next++ = '\0';
		line += strspn(line, " \t");
		if (!in) {
			in = strcmp(line, "Repositories:") == 0;
			continue;
		}
		/* "NAME: {" opens a repository, "enabled : yes," inside. */
		if ((s = strstr(line, ": {")) != NULL) {
			snprintf(name, sizeof(name), "%.*s", (int)(s - line),
			    line);
		} else if (strncmp(line, "enabled", 7) == 0 && name[0] !=
		    '\0') {
			if (strstr(line, "yes") != NULL)
				add_repo(ps, name);
			name[0] = '\0';
		}
	}
out:
	buf_free(&b);
}

/*
 * dpkg: only installed packages count (not those removed with their
 * configuration left); required, important and essential ones make up
 * the base system. dpkg keeps no install time: the file list of the
 * package is written when it is unpacked.
 */
static int
dpkg_row(struct package *pk, char **f)
{
	char prio[16] = "", ess[8] = "", arch[16] = "", path[256];
	const char *h = f[F_HINT];
	struct stat st;

	if (strlen(h) < 3 || (h[0] != 'i' && h[0] != 'h') || h[1] != 'i')
		return -1;
	sscanf(h + 3, "%15s %7s %15s", prio, ess, arch);
	pk->kind = strcmp(prio, "required") == 0 ||
	    strcmp(prio, "important") == 0 || strcmp(ess, "yes") == 0 ?
	    PKG_SYSTEM : PKG_MANUAL;
	snprintf(path, sizeof(path), "/var/lib/dpkg/info/%s:%s.list",
	    pk->name, arch);
	if (stat(path, &st) == 0 || (snprintf(path, sizeof(path),
	    "/var/lib/dpkg/info/%s.list", pk->name), stat(path, &st) == 0))
		pk->installed = st.st_mtime;
	return 0;
}

/* Keep the first suite word of an apt source line: "host suite". */
static void
apt_add(struct packages *ps, const char *uri, const char *suites)
{
	char entry[128];
	const char *host = strstr(uri, "://");
	int n;

	host = host != NULL ? host + 3 : uri;
	n = (int)strcspn(host, "/");
	snprintf(entry, sizeof(entry), "%.*s %.*s", n, host,
	    (int)strcspn(suites, " \t\n"), suites);
	add_repo(ps, entry);
}

/* One-line sources: "deb [options] uri suite components". */
static void
apt_list(struct packages *ps, FILE *fp)
{
	char *line = NULL, *p, *uri;
	size_t cap = 0;

	while (getline(&line, &cap, fp) != -1) {
		p = line + strspn(line, " \t");
		if (strncmp(p, "deb", 3) != 0 || (p[3] != ' ' && p[3] != '\t'))
			continue;	/* comments and deb-src */
		p += 3 + strspn(p + 3, " \t");
		if (*p == '[' && (p = strchr(p, ']')) != NULL)
			p++;
		if (p == NULL)
			continue;
		uri = p + strspn(p, " \t");
		p = uri + strcspn(uri, " \t\n");
		if (*p == '\0')
			continue;
		*p++ = '\0';
		apt_add(ps, uri, p + strspn(p, " \t"));
	}
	free(line);
}

/* deb822 sources: stanzas of "Types:", "URIs:", "Suites:", "Enabled:". */
static void
apt_sources(struct packages *ps, FILE *fp)
{
	char *line = NULL, uri[128] = "", suites[64] = "", *v;
	size_t cap = 0;
	int deb = 0, off = 0, end;

	do {
		end = getline(&line, &cap, fp) == -1;
		if (end || line[strspn(line, " \t\n")] == '\0') {
			if (deb && !off && uri[0] != '\0')
				apt_add(ps, uri, suites);
			deb = off = 0;
			uri[0] = suites[0] = '\0';
			continue;
		}
		if (line[0] == '#' || (v = strchr(line, ':')) == NULL)
			continue;
		*v++ = '\0';
		v += strspn(v, " \t");
		v[strcspn(v, "\n")] = '\0';
		if (strcasecmp(line, "Types") == 0)
			deb = has_word(v, "deb", " \t");
		else if (strcasecmp(line, "URIs") == 0)
			snprintf(uri, sizeof(uri), "%.*s",
			    (int)strcspn(v, " \t"), v);
		else if (strcasecmp(line, "Suites") == 0)
			snprintf(suites, sizeof(suites), "%s", v);
		else if (strcasecmp(line, "Enabled") == 0)
			off = strcasecmp(v, "no") == 0;
	} while (!end);
	free(line);
}

/* Every file of a directory with the given suffix, sorted by name. */
static void
each_file(struct packages *ps, const char *dir, const char *suffix,
    void (*fn)(struct packages *, FILE *))
{
	struct dirent **ents;
	char path[512];
	size_t n, sl = strlen(suffix);
	FILE *fp;
	int i, k;

	if ((k = scandir(dir, &ents, NULL, alphasort)) < 0)
		return;
	for (i = 0; i < k; i++) {
		n = strlen(ents[i]->d_name);
		if (n > sl && strcmp(ents[i]->d_name + n - sl, suffix) == 0) {
			snprintf(path, sizeof(path), "%s/%s", dir,
			    ents[i]->d_name);
			if ((fp = fopen(path, "r")) != NULL) {
				fn(ps, fp);
				fclose(fp);
			}
		}
		free(ents[i]);
	}
	free(ents);
}

/* apt marks what it installed as a dependency in extended_states. */
static void
dpkg_finish(struct packages *ps)
{
	char *line = NULL, name[128] = "";
	struct package *pk;
	size_t cap = 0;
	FILE *fp;

	if ((fp = fopen("/var/lib/apt/extended_states", "r")) != NULL) {
		while (getline(&line, &cap, fp) != -1) {
			if (sscanf(line, "Package: %127s", name) == 1)
				continue;
			if (strncmp(line, "Auto-Installed: 1", 17) == 0 &&
			    (pk = find_name(ps, name)) != NULL &&
			    pk->kind == PKG_MANUAL)
				pk->kind = PKG_DEPENDENCY;
		}
		free(line);
		fclose(fp);
	}
	if ((fp = fopen("/etc/apt/sources.list", "r")) != NULL) {
		apt_list(ps, fp);
		fclose(fp);
	}
	each_file(ps, "/etc/apt/sources.list.d", ".list", apt_list);
	each_file(ps, "/etc/apt/sources.list.d", ".sources", apt_sources);
}

/* yum, dnf and zypper repositories: "[id]" sections with "enabled=". */
static void
ini_repos(struct packages *ps, FILE *fp)
{
	char *line = NULL, id[64] = "";
	size_t cap = 0;
	int on = 1, end;

	do {
		end = getline(&line, &cap, fp) == -1;
		if (end || line[0] == '[') {
			if (id[0] != '\0' && on)
				add_repo(ps, id);
			on = 1;	/* the default when the key is missing */
			if (!end)
				sscanf(line, "[%63[^]]", id);
		} else if (strncmp(line, "enabled", 7) == 0) {
			on = strchr(line, '1') != NULL;
		}
	} while (!end);
	free(line);
}

/* zypper lists what it installed as a dependency, one name per line. */
static void
rpm_finish(struct packages *ps)
{
	char *line = NULL;
	struct package *pk;
	size_t cap = 0, i;
	FILE *fp;

	if ((fp = fopen("/var/lib/zypp/AutoInstalled", "r")) != NULL) {
		for (i = 0; i < ps->n; i++)
			ps->p[i].kind = PKG_MANUAL;
		while (getline(&line, &cap, fp) != -1) {
			line[strcspn(line, "\n")] = '\0';
			if (line[0] != '#' && (pk = find_name(ps, line)) != NULL)
				pk->kind = PKG_DEPENDENCY;
		}
		free(line);
		fclose(fp);
	}
	each_file(ps, "/etc/yum.repos.d", ".repo", ini_repos);
	each_file(ps, "/etc/zypp/repos.d", ".repo", ini_repos);
}

int
pkgs_load(struct packages *ps)
{
	const char *fake = getenv("NASCHECK_PKG");	/* tests: a stand-in */
	const struct manager *m;
	char *argv[6];
	size_t i;
	int code;

	memset(ps, 0, sizeof(*ps));
	for (m = managers; m < managers + sizeof(managers) /
	    sizeof(managers[0]); m++) {
		memcpy(argv, m->argv, sizeof(argv));
		if (fake != NULL && *fake != '\0') {
			if (m > managers)
				break;	/* the stand-in speaks pkg */
			argv[0] = (char *)(pkg_tool = fake);
		}
		if (run_capture(argv, &ps->b, 1, &code) == -1 || code != 0)
			continue;
		ps->manager = m->name;
		if (parse(ps, m) == -1)
			return -1;
		m->finish(ps);
		for (i = 0; i < ps->n; i++)
			ps->count[ps->p[i].kind]++;
		return 0;
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
