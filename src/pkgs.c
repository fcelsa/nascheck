/* pkgs.c - the packages installed on the host: from its package manager,
 * then flatpak and snap. */
#include <sys/stat.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "pkgs.h"

/*
 * Package managers that print their packages (pkg, dpkg, rpm) print the
 * same tab-separated fields, empty where they have nothing to say:
 *
 *   name version size time automatic rdeps repository hint description
 *
 * "hint" is what the manager knows about the kind of package; row() reads
 * it, finish() adds what the manager keeps in files. pacman and apk keep
 * plain text databases, read directly.
 */
enum { F_NAME, F_VERSION, F_SIZE, F_TIME, F_AUTO, F_RDEPS, F_REPO, F_HINT,
    F_COMMENT, NFIELDS };

struct manager {
	const char	*name;
	int		(*load)(struct packages *ps, const struct manager *m);
	char		*argv[6];
	uint64_t	 size_unit;	/* bytes per size unit */
	int		(*row)(struct package *pk, char **f);	/* -1: skip */
	void		(*finish)(struct packages *ps);
};

static int	command_load(struct packages *, const struct manager *);
static int	pkg_row(struct package *, char **);
static void	pkg_finish(struct packages *);
static int	dpkg_row(struct package *, char **);
static void	dpkg_finish(struct packages *);
static int	pacman_load(struct packages *, const struct manager *);
static int	apk_load(struct packages *, const struct manager *);
static void	rpm_finish(struct packages *);

static const struct manager managers[] = {
	{ "pkg", command_load, { "pkg", "query",
	    "%n\t%v\t%sb\t%t\t%a\t%#r\t%R\t%o\t%c", NULL }, 1,
	    pkg_row, pkg_finish },
	/* The hint: status, priority, essential, architecture. */
	{ "dpkg", command_load, { "dpkg-query", "-W", "-f",
	    "${Package}\t${Version}\t${Installed-Size}\t\t\t\t\t"
	    "${db:Status-Abbrev}${Priority} ${Essential} ${Architecture}\t"
	    "${binary:Summary}\n", NULL }, 1024, dpkg_row, dpkg_finish },
	{ "pacman", pacman_load, { NULL }, 1, NULL, NULL },
	{ "apk", apk_load, { NULL }, 1, NULL, NULL },
	{ "rpm", command_load, { "rpm", "-qa", "--qf", "%{NAME}\t"
	    "%{VERSION}-%{RELEASE}\t%{SIZE}\t%{INSTALLTIME}\t\t\t\t\t"
	    "%{SUMMARY}\n", NULL }, 1, NULL, rpm_finish },
};
#define NMANAGERS	(sizeof(managers) / sizeof(managers[0]))

/* Tests: NASCHECK_ROOT stands for /, with stand-in commands in its bin/. */
static const char *root = "";

/* A file of the host, under the test root if there is one. */
static const char *
host_path(char *dst, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n = snprintf(dst, size, "%s", root);

	if (n >= 0 && (size_t)n < size) {
		va_start(ap, fmt);
		vsnprintf(dst + n, size - (size_t)n, fmt, ap);
		va_end(ap);
	}
	return dst;
}

static FILE *
host_open(const char *file)
{
	char path[512];

	return fopen(host_path(path, sizeof(path), "%s", file), "r");
}

/* Run a command into b; 0 only if it ran and exited with 0. */
static int
run(char **argv, struct buf *b)
{
	char cmd[512], *name = argv[0];
	int code, rc;

	if (*root != '\0') {
		snprintf(cmd, sizeof(cmd), "%s/bin/%s", root, name);
		if (access(cmd, X_OK) != 0)
			return -1;
		argv[0] = cmd;
	}
	rc = run_capture(argv, b, 1, &code);
	argv[0] = name;
	return rc == -1 || code != 0 ? -1 : 0;
}

/* The next line of a buffer split in place, or NULL at its end. */
static char *
next_line(char **cur)
{
	char *line = *cur, *nl;

	if (line == NULL || *line == '\0')
		return NULL;
	if ((nl = strchr(line, '\n')) != NULL) {
		*nl = '\0';
		*cur = nl + 1;
	} else {
		*cur = line + strlen(line);
	}
	return line;
}

/* A new package, everything unknown. */
static struct package *
add_pkg(struct packages *ps)
{
	struct package *pk;
	size_t cap;
	void *p;

	if (ps->n == ps->cap) {
		cap = ps->cap ? ps->cap * 2 : 512;
		if ((p = realloc(ps->p, cap * sizeof(*ps->p))) == NULL)
			return NULL;
		ps->p = p;
		ps->cap = cap;
	}
	pk = &ps->p[ps->n++];
	memset(pk, 0, sizeof(*pk));
	pk->version = pk->comment = pk->repo = "";
	pk->rdeps = -1;
	return pk;
}

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

/* A dependency without its version or tag: "glibc>=2.40" is "glibc". */
static void
mark(struct packages *ps, char *dep, int kind)
{
	struct package *pk;

	dep[strcspn(dep, "<>=~:@ \t")] = '\0';
	if ((pk = find_name(ps, dep)) != NULL)
		pk->kind = kind;
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

/* Every file of a directory with the given suffix, sorted by name. */
static void
each_file(struct packages *ps, const char *dir, const char *suffix,
    void (*fn)(struct packages *, FILE *))
{
	struct dirent **ents;
	char path[512], file[768];
	size_t n, sl = strlen(suffix);
	FILE *fp;
	int i, k;

	if ((k = scandir(host_path(path, sizeof(path), "%s", dir), &ents,
	    NULL, alphasort)) < 0)
		return;
	for (i = 0; i < k; i++) {
		n = strlen(ents[i]->d_name);
		if (n > sl && strcmp(ents[i]->d_name + n - sl, suffix) == 0) {
			snprintf(file, sizeof(file), "%s/%s", path,
			    ents[i]->d_name);
			if ((fp = fopen(file, "r")) != NULL) {
				fn(ps, fp);
				fclose(fp);
			}
		}
		free(ents[i]);
	}
	free(ents);
}

/* "[id]" sections, with "enabled=" (zypper) or not (pacman). */
static void
ini_repos(struct packages *ps, FILE *fp)
{
	char *line = NULL, id[64] = "";
	size_t cap = 0;
	int on = 1, end;

	do {
		end = getline(&line, &cap, fp) == -1;
		if (end || line[0] == '[') {
			if (id[0] != '\0' && on && strcmp(id, "options") != 0)
				add_repo(ps, id);
			on = 1;	/* the default when the key is missing */
			id[0] = '\0';
			if (!end)
				sscanf(line, "[%63[^]]", id);
		} else if (strncmp(line, "enabled", 7) == 0) {
			on = strchr(line, '1') != NULL;
		}
	} while (!end);
	free(line);
}

/* A manager that prints its packages: split the output in place. */
static int
command_load(struct packages *ps, const struct manager *m)
{
	char *argv[6], *cur, *line, *f[NFIELDS];
	struct package *pk;
	int k;

	memcpy(argv, m->argv, sizeof(argv));
	if (run(argv, &ps->b[PB_MANAGER]) == -1)
		return -1;
	for (cur = ps->b[PB_MANAGER].data; (line = next_line(&cur)) != NULL;) {
		for (k = 0; k < NFIELDS && line != NULL; k++) {
			f[k] = line;
			if (k < NFIELDS - 1 && (line = strchr(line, '\t')) !=
			    NULL)
				*line++ = '\0';
		}
		if (k < NFIELDS || f[F_NAME][0] == '\0')
			continue;
		if ((pk = add_pkg(ps)) == NULL)
			return -1;
		pk->name = f[F_NAME];
		pk->version = f[F_VERSION];
		pk->comment = f[F_COMMENT];
		pk->repo = f[F_REPO];
		pk->size = strtoull(f[F_SIZE], NULL, 10) * m->size_unit;
		pk->installed = (time_t)strtoll(f[F_TIME], NULL, 10);
		if (f[F_RDEPS][0] != '\0')
			pk->rdeps = atoi(f[F_RDEPS]);
		if (m->row != NULL && m->row(pk, f) == -1)
			ps->n--;
	}
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
	char *argv[] = { "pkg", "-vv", NULL }, *cur, *line, *s;
	char name[64] = "";
	struct buf b = { 0 };
	size_t i;
	int in = 0;

	for (i = 0; i < ps->n && ps->p[i].kind != PKG_SYSTEM; i++)
		;
	ps->base = i < ps->n ? "pkgbase" : "freebsd-update";
	if (run(argv, &b) == -1)
		goto out;
	for (cur = b.data; (line = next_line(&cur)) != NULL;) {
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
	char prio[16] = "", ess[8] = "", arch[16] = "", path[512];
	const char *h = f[F_HINT];
	struct stat st;

	if (strlen(h) < 3 || (h[0] != 'i' && h[0] != 'h') || h[1] != 'i')
		return -1;
	sscanf(h + 3, "%15s %7s %15s", prio, ess, arch);
	pk->kind = strcmp(prio, "required") == 0 ||
	    strcmp(prio, "important") == 0 || strcmp(ess, "yes") == 0 ?
	    PKG_SYSTEM : PKG_MANUAL;
	if (stat(host_path(path, sizeof(path), "/var/lib/dpkg/info/%s:%s.list",
	    pk->name, arch), &st) == 0 || stat(host_path(path, sizeof(path),
	    "/var/lib/dpkg/info/%s.list", pk->name), &st) == 0)
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

/* apt marks what it installed as a dependency in extended_states. */
static void
dpkg_finish(struct packages *ps)
{
	char *line = NULL, name[128] = "";
	struct package *pk;
	size_t cap = 0;
	FILE *fp;

	if ((fp = host_open("/var/lib/apt/extended_states")) != NULL) {
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
	if ((fp = host_open("/etc/apt/sources.list")) != NULL) {
		apt_list(ps, fp);
		fclose(fp);
	}
	each_file(ps, "/etc/apt/sources.list.d", ".list", apt_list);
	each_file(ps, "/etc/apt/sources.list.d", ".sources", apt_sources);
}

/*
 * pacman: one "desc" file per package, sections like "%NAME%" followed by
 * their values. %REASON% 1 means a dependency. The "base" meta package
 * and what it depends on make up the base system.
 */
static int
pacman_load(struct packages *ps, const struct manager *m)
{
	struct buf *b = &ps->b[PB_MANAGER];
	char dir[512], file[800], *cur, *line, *sect = "", *deps = NULL, *next;
	struct package *pk = NULL;
	struct dirent *e;
	size_t ndeps = 0;
	FILE *fp;
	DIR *d;

	(void)m;
	if ((d = opendir(host_path(dir, sizeof(dir),
	    "/var/lib/pacman/local"))) == NULL)
		return -1;
	while ((e = readdir(d)) != NULL)
		if (e->d_name[0] != '.') {
			snprintf(file, sizeof(file), "%s/%s/desc", dir,
			    e->d_name);
			if (buf_append_file(b, file) == 0)
				buf_append(b, "\n", 1);	/* keep apart */
		}
	closedir(d);
	for (cur = b->data; (line = next_line(&cur)) != NULL;) {
		if (line[0] == '%') {
			sect = line;
		} else if (line[0] == '\0') {
			continue;
		} else if (strcmp(sect, "%NAME%") == 0) {
			if ((pk = add_pkg(ps)) == NULL)
				return -1;
			pk->name = line;
			pk->kind = PKG_MANUAL;
		} else if (pk == NULL) {
			continue;
		} else if (strcmp(sect, "%VERSION%") == 0) {
			pk->version = line;
		} else if (strcmp(sect, "%DESC%") == 0) {
			pk->comment = line;
		} else if (strcmp(sect, "%INSTALLDATE%") == 0) {
			pk->installed = (time_t)strtoll(line, NULL, 10);
		} else if (strcmp(sect, "%SIZE%") == 0) {
			pk->size = strtoull(line, NULL, 10);
		} else if (strcmp(sect, "%REASON%") == 0) {
			pk->kind = line[0] == '1' ? PKG_DEPENDENCY : PKG_MANUAL;
		} else if (strcmp(sect, "%DEPENDS%") == 0 &&
		    strcmp(pk->name, "base") == 0) {
			if (deps == NULL)
				deps = line;	/* the next ones follow it */
			ndeps++;
		}
	}
	qsort(ps->p, ps->n, sizeof(*ps->p), by_name);
	mark(ps, (char[]){ "base" }, PKG_SYSTEM);
	for (; ndeps > 0; ndeps--, deps = next) {
		next = deps + strlen(deps) + 1;
		mark(ps, deps, PKG_SYSTEM);
	}
	if ((fp = host_open("/etc/pacman.conf")) != NULL) {
		ini_repos(ps, fp);
		fclose(fp);
	}
	return 0;
}

/* An apk repository as its last two path parts: "v3.21/main". */
static void
apk_repo(struct packages *ps, char *line)
{
	char *p, *url = line;
	int parts = 0;

	for (p = line; *p != '\0'; p++)	/* the last word: "@tag url" */
		if (*p == ' ' || *p == '\t')
			url = p + 1;
	for (p = url + strlen(url); p > url && p[-1] == '/'; )
		*--p = '\0';
	for (p = url + strlen(url); p > url; p--)
		if (p[-1] == '/' && ++parts == 2)
			break;
	add_repo(ps, p);
}

/*
 * apk: stanzas of "X:value" lines in the installed database (P name,
 * V version, T description, I size, D dependencies). /etc/apk/world lists
 * what was asked for; alpine-base and what it depends on are the base
 * system. apk keeps no install time.
 */
static int
apk_load(struct packages *ps, const struct manager *m)
{
	struct buf *b = &ps->b[PB_MANAGER];
	char path[512], *cur, *line, *base = NULL, *w, *save;
	struct package *pk = NULL;
	FILE *fp;

	(void)m;
	if (buf_append_file(b, host_path(path, sizeof(path),
	    "/lib/apk/db/installed")) == -1)
		return -1;
	for (cur = b->data; (line = next_line(&cur)) != NULL;) {
		if (line[0] == '\0' || line[1] != ':')
			continue;
		if (line[0] == 'P') {
			if ((pk = add_pkg(ps)) == NULL)
				return -1;
			pk->name = line + 2;
			pk->kind = PKG_DEPENDENCY;
		} else if (pk == NULL) {
			continue;
		} else if (line[0] == 'V') {
			pk->version = line + 2;
		} else if (line[0] == 'T') {
			pk->comment = line + 2;
		} else if (line[0] == 'I') {
			pk->size = strtoull(line + 2, NULL, 10);
		} else if (line[0] == 'D' && strcmp(pk->name,
		    "alpine-base") == 0) {
			base = line + 2;
		}
	}
	qsort(ps->p, ps->n, sizeof(*ps->p), by_name);
	if ((fp = host_open("/etc/apk/world")) != NULL) {
		while (fscanf(fp, "%255s", path) == 1)
			mark(ps, path, PKG_MANUAL);
		fclose(fp);
	}
	mark(ps, (char[]){ "alpine-base" }, PKG_SYSTEM);
	for (w = base != NULL ? strtok_r(base, " ", &save) : NULL; w != NULL;
	    w = strtok_r(NULL, " ", &save))
		if (*w != '!')		/* "!name" is a conflict */
			mark(ps, w, PKG_SYSTEM);
	if ((fp = host_open("/etc/apk/repositories")) != NULL) {
		while (fgets(path, sizeof(path), fp) != NULL) {
			path[strcspn(path, "\r\n")] = '\0';
			if (path[0] != '#' && path[strspn(path, " \t")] != '\0')
				apk_repo(ps, path);
		}
		fclose(fp);
	}
	return 0;
}

/*
 * dnf keeps why each package was installed: "user", "dependency", "weak
 * dependency", or "group" (what the installer put in: the base system).
 * Only the local state is read (-C, no repository).
 */
static int
dnf_reasons(struct packages *ps)
{
	static char qf[] = "%{name}\t%{reason}\t%{from_repo}\n";
	char *dnf5[] = { "dnf5", "-q", "-C", "--disable-repo=*", "repoquery",
	    "--installed", "--qf", qf, NULL };
	char *dnf4[] = { "dnf", "-q", "-C", "--disablerepo=*", "repoquery",
	    "--installed", "--qf", qf, NULL };
	struct buf *b = &ps->b[PB_DNF];
	struct package *pk;
	char *cur, *line, *reason, *repo;

	if (run(dnf5, b) == -1 && run(dnf4, b) == -1)
		return -1;
	for (cur = b->data; (line = next_line(&cur)) != NULL;) {
		if ((reason = strchr(line, '\t')) == NULL)
			continue;
		*reason++ = '\0';
		if ((repo = strchr(reason, '\t')) != NULL)
			*repo++ = '\0';
		if ((pk = find_name(ps, line)) == NULL)
			continue;
		if (strncasecmp(reason, "user", 4) == 0)
			pk->kind = PKG_MANUAL;
		else if (strncasecmp(reason, "group", 5) == 0)
			pk->kind = PKG_SYSTEM;
		else if (strstr(reason, "ependency") != NULL)	/* any case */
			pk->kind = PKG_DEPENDENCY;
		if (repo != NULL && repo[0] != '<' && repo[0] != '@')
			pk->repo = repo;
	}
	return 0;
}

/*
 * The enabled repositories as dnf sees them: their files moved over time
 * (/etc/yum.repos.d, /usr/share/dnf5/repos.d) and overrides change them.
 * "repolist" reads the configuration only.
 */
static int
dnf_repos(struct packages *ps)
{
	char *dnf5[] = { "dnf5", "-q", "-C", "repolist", "--enabled", NULL };
	char *dnf4[] = { "dnf", "-q", "-C", "repolist", "--enabled", NULL };
	char *cur, *line;
	struct buf b = { 0 };
	int rc = -1;

	if (run(dnf5, &b) == 0 || run(dnf4, &b) == 0) {
		rc = 0;
		for (cur = b.data; (line = next_line(&cur)) != NULL;) {
			line[strcspn(line, " \t")] = '\0';
			if (strcmp(line, "repo") != 0)	/* "repo id  repo name" */
				add_repo(ps, line);
		}
	}
	buf_free(&b);
	return rc;
}

/* rpm itself keeps no reason: ask dnf, or read zypper's list. */
static void
rpm_finish(struct packages *ps)
{
	char *line = NULL;
	struct package *pk;
	size_t cap = 0, i;
	FILE *fp;

	if (dnf_reasons(ps) == 0) {
		dnf_repos(ps);
		return;
	}
	if ((fp = host_open("/var/lib/zypp/AutoInstalled")) != NULL) {
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
	each_file(ps, "/etc/zypp/repos.d", ".repo", ini_repos);
}

/* "1.2 GB", "345,6 MB" (the decimal mark follows the locale): bytes. */
static uint64_t
human_size(const char *s)
{
	char *end;
	double v = (double)strtoull(s, &end, 10), scale = 0.1;

	if (*end == '.' || *end == ',')
		for (end++; isdigit((unsigned char)*end); end++, scale /= 10)
			v += (*end - '0') * scale;
	while (*end != '\0' && !isalpha((unsigned char)*end))
		end++;		/* spaces, maybe non-breaking */
	switch (toupper((unsigned char)*end)) {
	case 'K':
		return (uint64_t)(v * 1e3);
	case 'M':
		return (uint64_t)(v * 1e6);
	case 'G':
		return (uint64_t)(v * 1e9);
	case 'T':
		return (uint64_t)(v * 1e12);
	}
	return (uint64_t)v;
}

/*
 * flatpak: applications were asked for, runtimes came with them. The
 * ref ("app/org.gnome.Maps/x86_64/stable") gives the kind and the ID.
 */
static void
flatpak_add(struct packages *ps)
{
	char *argv[] = { "flatpak", "list",
	    "--columns=ref,version,size,origin,description", NULL };
	char *cur, *line, *f[5], *id, *slash, path[512];
	const char *home = *root == '\0' ? getenv("HOME") : NULL;
	struct package *pk;
	struct stat st;
	int k, app;

	if (run(argv, &ps->b[PB_FLATPAK]) == -1)
		return;
	for (cur = ps->b[PB_FLATPAK].data; (line = next_line(&cur)) != NULL;) {
		for (k = 0; k < 5 && line != NULL; k++) {
			f[k] = line;
			if (k < 4 && (line = strchr(line, '\t')) != NULL)
				*line++ = '\0';
		}
		if (k < 5 || ((app = strncmp(f[0], "app/", 4) == 0) == 0 &&
		    strncmp(f[0], "runtime/", 8) != 0))
			continue;	/* a header, or not a ref */
		id = strchr(f[0], '/') + 1;
		if ((slash = strchr(id, '/')) != NULL)
			*slash = '\0';
		if ((pk = add_pkg(ps)) == NULL)
			return;
		pk->name = id;
		/* Runtimes have no version: their branch, at the ref's end. */
		pk->version = f[1][0] != '\0' || slash == NULL ? f[1] :
		    strrchr(slash + 1, '/') != NULL ? strrchr(slash + 1, '/') + 1 :
		    slash + 1;
		pk->size = human_size(f[2]);
		pk->repo = f[3];
		pk->comment = f[4];
		pk->source = "flatpak";
		pk->kind = app ? PKG_MANUAL : PKG_DEPENDENCY;
		/* No install date: its directory changes when it is deployed. */
		if (stat(host_path(path, sizeof(path), "/var/lib/flatpak/%s/%s",
		    app ? "app" : "runtime", id), &st) == 0 || (home != NULL &&
		    snprintf(path, sizeof(path), "%s/.local/share/flatpak/%s/%s",
		    home, app ? "app" : "runtime", id) > 0 &&
		    stat(path, &st) == 0))
			pk->installed = st.st_mtime;
		ps->nflatpak++;
	}
}

/* The summary in a snap's metadata, appended to b; "" if none. */
static void
snap_summary(const char *name, struct buf *tmp, struct buf *b)
{
	static const char *const dirs[] = { "/snap", "/var/lib/snapd/snap" };
	char path[512], *p, *end;
	size_t i;

	for (i = 0; i < 2; i++) {
		tmp->len = 0;
		if (buf_append_file(tmp, host_path(path, sizeof(path),
		    "%s/%s/current/meta/snap.yaml", dirs[i], name)) == 0)
			break;
	}
	p = i < 2 && tmp->data != NULL ? strstr(tmp->data, "\nsummary:") :
	    NULL;
	if (p != NULL) {
		p += 9 + strspn(p + 9, " \t'\"");
		end = p + strcspn(p, "\n");
		while (end > p && (end[-1] == '\'' || end[-1] == '"' ||
		    end[-1] == ' '))
			end--;
		buf_append(b, p, (size_t)(end - p));
	}
	buf_append(b, "", 1);
}

/*
 * snap: "snap list" columns are name, version, rev, tracking, publisher,
 * notes. Bases came with the applications; snapd, core, kernel and gadget
 * snaps are the system. Size and date are the snap file's.
 */
static void
snap_add(struct packages *ps)
{
	char *argv[] = { "snap", "list", NULL }, *cur, *line, *f[6], *save;
	char path[512];
	struct buf *info = &ps->b[PB_SNAPINFO], tmp = { 0 };
	struct package *pk;
	struct stat st;
	size_t first = ps->n, *off = NULL, n = 0, i;
	void *p;
	int k;

	if (run(argv, &ps->b[PB_SNAP]) == -1)
		return;
	for (cur = ps->b[PB_SNAP].data; (line = next_line(&cur)) != NULL;) {
		f[0] = strtok_r(line, " \t", &save);
		for (k = 1; k < 6 && (f[k] = strtok_r(NULL, " \t", &save)) !=
		    NULL; k++)
			;
		if (k < 6 || strcmp(f[0], "Name") == 0)
			continue;
		if ((p = realloc(off, (n + 1) * sizeof(*off))) == NULL ||
		    (pk = add_pkg(ps)) == NULL)
			break;
		off = p;
		pk->name = f[0];
		pk->version = f[1];
		pk->repo = strcmp(f[3], "-") != 0 ? f[3] : "";
		pk->source = "snap";
		pk->kind = has_word(f[5], "base", ",") ? PKG_DEPENDENCY :
		    has_word(f[5], "snapd", ",") || has_word(f[5], "core", ",") ||
		    has_word(f[5], "kernel", ",") || has_word(f[5], "gadget",
		    ",") ? PKG_SYSTEM : PKG_MANUAL;
		if (stat(host_path(path, sizeof(path),
		    "/var/lib/snapd/snaps/%s_%s.snap", f[0], f[2]), &st) == 0) {
			pk->size = (uint64_t)st.st_size;
			pk->installed = st.st_mtime;
		}
		off[n++] = info->len;
		snap_summary(f[0], &tmp, info);
	}
	/* The summaries are in place now: point to them. */
	for (i = 0; i < n && info->data != NULL; i++)
		ps->p[first + i].comment = info->data + off[i];
	ps->nsnap = n;
	free(off);
	buf_free(&tmp);
}

int
pkgs_load(struct packages *ps)
{
	const char *r = getenv("NASCHECK_ROOT");
	const struct manager *m;
	size_t i;

	memset(ps, 0, sizeof(*ps));
	root = r != NULL ? r : "";
	for (m = managers; m < managers + NMANAGERS; m++) {
		if (m->load(ps, m) == 0 && ps->n > 0)
			break;
		ps->n = 0;	/* a manager with no package does not count */
		ps->b[PB_MANAGER].len = 0;
	}
	if (m < managers + NMANAGERS) {
		ps->manager = m->name;
		qsort(ps->p, ps->n, sizeof(*ps->p), by_name);
		if (m->finish != NULL)
			m->finish(ps);
	}
	flatpak_add(ps);
	snap_add(ps);
	qsort(ps->p, ps->n, sizeof(*ps->p), by_name);
	for (i = 0; i < ps->n; i++)
		ps->count[ps->p[i].kind]++;
	if (ps->n == 0) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

void
pkgs_free(struct packages *ps)
{
	int i;

	for (i = 0; i < PB_COUNT; i++)
		buf_free(&ps->b[i]);
	free(ps->p);
	memset(ps, 0, sizeof(*ps));
}
