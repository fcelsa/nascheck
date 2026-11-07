/* nascheck - disk health monitor for FreeBSD and Linux. */
#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "daemon.h"
#include "db.h"
#include "health.h"
#include "smart.h"

#ifndef NASCHECK_VERSION
#define NASCHECK_VERSION "dev"
#endif
#ifndef NASCHECK_CONF
#define NASCHECK_CONF "/usr/local/etc/nascheck.conf"
#endif

#define EXIT_USAGE	64	/* EX_USAGE from sysexits(3) */
#define EXIT_UNKNOWN	3	/* check could not run (monitoring plugin API) */

static void
usage(FILE *f)
{
	fprintf(f,
	    "usage: nascheck [-hlqV] [-c config] command [device ...]\n"
	    "\n"
	    "commands:\n"
	    "  scan               list the devices reported by smartctl\n"
	    "  status [device]    table of all or the given disks\n"
	    "  check [device]     report problems; exit 0 ok, 1 warning,\n"
	    "                     2 critical, 3 unknown\n"
	    "  daemon             collect data in the background (as root)\n"
	    "  refresh            ask the daemon to read all disks now\n"
	    "\n"
	    "  -c config  configuration file (default " NASCHECK_CONF ")\n"
	    "  -l         status, check: read the disks, not the database\n"
	    "  -q         check: print the summary line only\n");
}

/* Decimal units, as printed on disk labels: 4.0T, 960G. */
static const char *
fmt_size(uint64_t bytes, char *buf, size_t size)
{
	static const char units[] = "KMGTPE";
	double v = (double)bytes;
	int i = -1;

	if (bytes == 0)
		return "-";
	while (v >= 1000 && i < 5) {
		v /= 1000;
		i++;
	}
	if (i < 0)
		snprintf(buf, size, "%uB", (unsigned)bytes);
	else
		snprintf(buf, size, v < 10 ? "%.1f%c" : "%.0f%c", v, units[i]);
	return buf;
}

static const char *
disk_kind(const struct disk *d)
{
	if (d->proto == PROTO_NVME)
		return "NVMe";
	if (d->proto != PROTO_ATA || d->rotation_rate < 0)
		return "-";
	return d->rotation_rate == 0 ? "SSD" : "HDD";
}

static const char *
tri(int8_t v, const char *yes, const char *no)
{
	return v == 1 ? yes : v == 0 ? no : "-";
}

/* Short summary of the counters that matter for each kind of disk. */
static void
fmt_notes(const struct disk *d, char *buf, size_t size)
{
	const struct ata_attr *a;
	const struct nvme_health *nv = &d->u.nvme;
	size_t len = 0;

	buf[0] = '\0';
	if (d->state == READ_ERROR) {
		snprintf(buf, size, "%s", d->error);
		return;
	}
	if (d->state != READ_OK)
		return;
	if (d->proto == PROTO_NVME) {
		snprintf(buf, size, "used %u%%, spare %u%%, media err %lld",
		    nv->percentage_used, nv->available_spare,
		    (long long)nv->media_errors);
		if (nv->critical_warning != 0) {
			len = strlen(buf);
			snprintf(buf + len, size - len, ", crit 0x%02x",
			    nv->critical_warning);
		}
		return;
	}
	if (d->proto != PROTO_ATA) {
		snprintf(buf, size, "not ATA or NVMe, skipped");
		return;
	}
	if ((a = smart_ata_attr(d, ATTR_REALLOC)) != NULL)
		len += (size_t)snprintf(buf + len, size - len,
		    "realloc %lld", (long long)a->raw);
	if (len < size && (a = smart_ata_attr(d, ATTR_PENDING)) != NULL)
		len += (size_t)snprintf(buf + len, size - len,
		    "%spending %lld", len ? ", " : "", (long long)a->raw);
	if (len < size && (a = smart_ata_attr(d, ATTR_OFFLINE)) != NULL)
		snprintf(buf + len, size - len,
		    "%suncorr %lld", len ? ", " : "", (long long)a->raw);
}

static const char *
level_short(const struct disk *d, const struct health *h)
{
	if (d->state == READ_STANDBY || (d->state == READ_OK &&
	    d->proto == PROTO_OTHER))
		return "-";
	return h->level == LEVEL_CRIT ? "CRIT" :
	    h->level == LEVEL_WARN ? "WARN" : "OK";
}

static void
print_header(void)
{
	printf("%-8s %-6s %-4s %-24s %-20s %6s %5s %7s %-7s %6s %s\n",
	    "DEVICE", "HEALTH", "TYPE", "MODEL", "SERIAL", "SIZE", "TEMP",
	    "HOURS", "SMART", "TEST", "NOTES");
}

/* Device name without the /dev/ prefix. */
static const char *
short_name(const struct disk *d)
{
	return strncmp(d->path, "/dev/", 5) == 0 ? d->path + 5 : d->path;
}

static void
print_disk(const struct disk *d, const struct health *h)
{
	const struct selftest *t = &d->last_test;
	const char *smart;
	char size[16], temp[8], hours[24], test[24], notes[128], line[384];
	char *end;

	fmt_size(d->capacity, size, sizeof(size));
	if (d->temp == TEMP_UNKNOWN)
		snprintf(temp, sizeof(temp), "-");
	else
		snprintf(temp, sizeof(temp), "%dC", d->temp);
	if (d->power_on_hours < 0)
		snprintf(hours, sizeof(hours), "-");
	else
		snprintf(hours, sizeof(hours), "%lld",
		    (long long)d->power_on_hours);
	/* Last self-test: FAIL, or how many power-on hours ago it ran. */
	if (t->valid && t->passed == 0)
		snprintf(test, sizeof(test), "FAIL");
	else if (t->valid && t->hours >= 0 && d->power_on_hours >= 0)
		snprintf(test, sizeof(test), "%lldh",
		    (long long)health_selftest_age(d));
	else
		snprintf(test, sizeof(test), "-");
	smart = d->state == READ_STANDBY ? "standby" :
	    d->state == READ_ERROR ? "error" :
	    tri(d->smart_passed, "PASSED", "FAILED");
	fmt_notes(d, notes, sizeof(notes));

	snprintf(line, sizeof(line),
	    "%-8s %-6s %-4s %-24.24s %-20.20s %6s %5s %7s %-7s %6s %s",
	    short_name(d), level_short(d, h), disk_kind(d),
	    d->model[0] ? d->model : "-", d->serial[0] ? d->serial : "-",
	    d->state == READ_OK ? size : "-", temp, hours, smart, test, notes);
	/* No trailing blanks when the notes are empty. */
	for (end = line + strlen(line); end > line && end[-1] == ' '; end--)
		;
	*end = '\0';
	puts(line);
}

/* The disks to report on, read live or loaded from the database. */
struct set {
	struct disk	*disks;
	time_t		*when;		/* report times, from the database */
	size_t		 n;
	time_t		 heartbeat;	/* last update by the daemon */
	int		 from_db;	/* loaded from the database */
	int		 partial;	/* some disk could not be read */
};

static void
set_free(struct set *s)
{
	free(s->disks);
	free(s->when);
	memset(s, 0, sizeof(*s));
}

/* Build the disk list from the command line, or scan when it is empty. */
static int
get_disks(struct buf *b, char **names, size_t count, struct disk **disks,
    size_t *n)
{
	size_t i;

	if (count == 0)
		return smart_scan(b, disks, n);
	for (i = 0; i < count; i++) {
		if (strlen(names[i]) >= sizeof((*disks)->path)) {
			fprintf(stderr, "nascheck: %s: name too long\n",
			    names[i]);
			return 2;
		}
	}
	if ((*disks = calloc(count, sizeof(**disks))) == NULL)
		return -1;
	for (i = 0; i < count; i++)
		strcpy((*disks)[i].path, names[i]);
	*n = count;
	return 0;
}

/*
 * Get the disks and read each one. Devices from a scan that are neither
 * ATA nor NVMe (USB sticks, SCSI) are dropped; named ones are kept.
 * Returns 0; 2 on a bad device name (reported); -1 on a fatal error left
 * in errno.
 */
static int
read_live(struct buf *b, char **names, size_t count, struct set *set)
{
	struct disk *d;
	size_t i, kept = 0;
	int rc;

	if ((rc = get_disks(b, names, count, &set->disks, &set->n)) != 0)
		return rc;
	for (i = 0; i < set->n; i++) {
		d = &set->disks[kept];
		if (i != kept)
			*d = set->disks[i];
		if (smart_read(b, d, READ_FULL) == -1) {
			/* Without smartctl there is nothing more to try. */
			if (errno == ENOENT)
				return -1;
			fprintf(stderr, "nascheck: %s: %s\n", d->path,
			    errno == EPROTO ? "smartctl output is not JSON" :
			    strerror(errno));
			set->partial = 1;
			continue;
		}
		if (count == 0 && d->state == READ_OK &&
		    d->proto == PROTO_OTHER)
			continue;
		kept++;
	}
	set->n = kept;
	return 0;
}

struct load_ctx {
	struct set	 *set;
	size_t		  cap;
	char		**names;	/* only these devices, if count > 0 */
	size_t		  count;
};

static int
load_report(void *arg, const struct db_report *r)
{
	struct load_ctx *ctx = arg;
	struct set *set = ctx->set;
	struct disk *d;
	void *p;
	size_t i;

	for (i = 0; i < ctx->count && strcmp(ctx->names[i], r->path) != 0; i++)
		;
	if (ctx->count > 0 && i == ctx->count)
		return 0;
	if (set->n == ctx->cap) {
		ctx->cap = ctx->cap ? ctx->cap * 2 : 16;
		if ((p = realloc(set->disks, ctx->cap * sizeof(*d))) == NULL)
			return -1;
		set->disks = p;
		if ((p = realloc(set->when, ctx->cap * sizeof(time_t))) == NULL)
			return -1;
		set->when = p;
	}
	d = &set->disks[set->n];
	if (r->json == NULL) {
		/* Never read: asleep or not readable since it appeared. */
		memset(d, 0, sizeof(*d));
		d->state = r->state;
		d->temp = d->temp_warn = d->temp_crit = TEMP_UNKNOWN;
		d->power_on_hours = d->rotation_rate = -1;
		d->smart_passed = -1;
		if (r->error != NULL)
			snprintf(d->error, sizeof(d->error), "%s", r->error);
	} else {
		snprintf(d->path, sizeof(d->path), "%s", r->path);
		if (smart_parse(r->json, r->len, 0, d) == -1)
			return 0;	/* a damaged report: skip the disk */
		/* The temperature sample is fresher than the daily report. */
		if (r->temp_time > r->time)
			d->temp = (int16_t)r->temp;
	}
	snprintf(d->path, sizeof(d->path), "%s", r->path);
	set->when[set->n++] = r->time;
	return 0;
}

/* Load the latest report of each present disk from the database. */
static int
read_db(const struct config *c, char **names, size_t count, struct set *set)
{
	struct load_ctx ctx = { set, 0, names, count };
	struct db db;
	int rc = 0;

	set->from_db = 1;
	if (db_open_reader(&db, c->database) == -1 ||
	    db_get_heartbeat(&db, &set->heartbeat) == -1 ||
	    db_each_report(&db, load_report, &ctx) == -1) {
		fprintf(stderr, "nascheck: %s: %s\n", c->database,
		    db_error(&db));
		fprintf(stderr, "nascheck: readers must be in the group set in "
		    "nascheck.conf; -l reads the disks directly (as root)\n");
		rc = 2;
	}
	db_close(&db);
	return rc;
}

/*
 * The database is used when it exists, unless -l asks for a live read.
 * Only a missing file means "no daemon": a permission problem is reported.
 */
static int
get_set(const struct config *c, struct buf *b, char **names, size_t count,
    int live, struct set *set)
{
	struct stat st;

	memset(set, 0, sizeof(*set));
	if (live || (stat(c->database, &st) == -1 && errno == ENOENT))
		return read_live(b, names, count, set);
	return read_db(c, names, count, set);
}

/* Minutes since the daemon last wrote, or -1 when it looks alive. */
static long
daemon_silence(const struct config *c, const struct set *set, time_t now)
{
	time_t limit = 2 * c->temp_interval + 60;

	if (!set->from_db || now - set->heartbeat <= limit)
		return -1;
	return set->heartbeat == 0 ? 0 : (long)(now - set->heartbeat) / 60;
}

/* Evaluate disk i; data from the database must also be recent enough. */
static void
judge(const struct config *c, const struct set *set, size_t i, time_t now,
    struct health *h)
{
	time_t age;

	health_eval(c, &set->disks[i], h);
	if (!set->from_db || set->disks[i].state != READ_OK)
		return;
	age = now - set->when[i];
	if (age > c->smart_interval + c->wake_after)
		health_add(h, LEVEL_WARN, "SMART data is %lld h old",
		    (long long)age / 3600);
}

/*
 * Commands return 0 on success, 1 on errors already reported, -1 on a fatal
 * error left in errno for main() to report.
 */
static int
cmd_scan(struct buf *b)
{
	struct disk *disks;
	size_t i, n;

	if (smart_scan(b, &disks, &n) == -1)
		return -1;
	for (i = 0; i < n; i++)
		printf("%s\n", disks[i].path);
	free(disks);
	return 0;
}

static int
cmd_status(const struct config *c, struct buf *b, char **names, size_t count,
    int live)
{
	struct set set;
	struct health h;
	time_t now = time(NULL);
	long silence;
	size_t i;
	int rc;

	if ((rc = get_set(c, b, names, count, live, &set)) != 0) {
		set_free(&set);
		return rc == 2 ? 1 : rc;
	}
	if ((silence = daemon_silence(c, &set, now)) >= 0)
		fprintf(stderr, "nascheck: no update from the daemon for "
		    "%ld min\n", silence);
	print_header();
	for (i = 0; i < set.n; i++) {
		judge(c, &set, i, now, &h);
		print_disk(&set.disks[i], &h);
	}
	rc = set.partial;
	set_free(&set);
	return rc;
}

/*
 * Summary line first, as monitoring systems read it, then one line per
 * finding. Returns the exit code: the worst level, or EXIT_UNKNOWN.
 */
static int
cmd_check(const struct config *c, struct buf *b, char **names, size_t count,
    int live, int quiet)
{
	struct set set;
	struct health *h = NULL;
	time_t now = time(NULL);
	size_t i, nwarn = 0, ncrit = 0;
	enum level worst = LEVEL_OK;
	long silence;
	uint8_t j;
	int rc, code, unknown;

	if ((rc = get_set(c, b, names, count, live, &set)) != 0) {
		set_free(&set);
		return rc == 2 ? EXIT_UNKNOWN : rc;
	}
	if (set.n > 0 && (h = calloc(set.n, sizeof(*h))) == NULL) {
		set_free(&set);
		return -1;
	}
	for (i = 0; i < set.n; i++) {
		judge(c, &set, i, now, &h[i]);
		nwarn += h[i].level == LEVEL_WARN;
		ncrit += h[i].level == LEVEL_CRIT;
		if (h[i].level > worst)
			worst = h[i].level;
	}
	/* Missing data makes the result unknown, unless already critical. */
	silence = daemon_silence(c, &set, now);
	unknown = set.partial || silence >= 0;
	code = worst == LEVEL_CRIT || !unknown ? (int)worst : EXIT_UNKNOWN;
	printf("%s: %zu disks, %zu critical, %zu warning%s\n",
	    code == EXIT_UNKNOWN ? "UNKNOWN" : level_name(worst), set.n, ncrit,
	    nwarn, set.partial ? ", some disks could not be read" : "");
	if (!quiet && silence >= 0)
		printf("%-8s %-8s no update from the daemon for %ld min\n",
		    "-", "UNKNOWN", silence);
	for (i = 0; i < set.n && !quiet; i++)
		for (j = 0; j < h[i].n; j++)
			printf("%-8s %-8s %s\n", short_name(&set.disks[i]),
			    level_name(h[i].f[j].level), h[i].f[j].text);
	free(h);
	set_free(&set);
	return code;
}

int
main(int argc, char **argv)
{
	struct config cfg;
	struct buf b = { 0 };
	const char *cmd, *conf = NULL;
	int ch, live = 0, quiet = 0, optional, rc;

	while ((ch = getopt(argc, argv, "c:hlqV")) != -1) {
		switch (ch) {
		case 'c':
			conf = optarg;
			break;
		case 'h':
			usage(stdout);
			return 0;
		case 'l':
			live = 1;
			break;
		case 'q':
			quiet = 1;
			break;
		case 'V':
			printf("nascheck %s\n", NASCHECK_VERSION);
			return 0;
		default:
			usage(stderr);
			return EXIT_USAGE;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc == 0) {
		usage(stderr);
		return EXIT_USAGE;
	}
	cmd = argv[0];

	if (strcmp(cmd, "scan") == 0 && argc == 1) {
		rc = cmd_scan(&b);
		goto out;
	}
	if (strcmp(cmd, "status") != 0 && strcmp(cmd, "check") != 0 &&
	    ((strcmp(cmd, "daemon") != 0 && strcmp(cmd, "refresh") != 0) ||
	    argc != 1)) {
		usage(stderr);
		return EXIT_USAGE;
	}

	/* The default file is optional, one given with -c is not. */
	config_defaults(&cfg);
	optional = conf == NULL;
	if (optional)
		conf = NASCHECK_CONF;
	if (config_load(&cfg, conf, optional) == -1)
		return strcmp(cmd, "check") == 0 ? EXIT_UNKNOWN : 1;

	if (strcmp(cmd, "daemon") == 0)
		return daemon_run(&cfg, conf);
	if (strcmp(cmd, "refresh") == 0)
		return daemon_refresh(&cfg) == 0 ? 0 : 1;
	if (strcmp(cmd, "check") == 0) {
		rc = cmd_check(&cfg, &b, argv + 1, (size_t)argc - 1, live,
		    quiet);
		if (rc == -1)
			fprintf(stderr, "nascheck: cannot use smartctl: %s\n",
			    strerror(errno));
		buf_free(&b);
		return rc == -1 ? EXIT_UNKNOWN : rc;
	}
	rc = cmd_status(&cfg, &b, argv + 1, (size_t)argc - 1, live);
out:
	if (rc == -1)
		fprintf(stderr, "nascheck: cannot use smartctl: %s\n",
		    errno == EPROTO ? "output is not JSON (smartmontools 7.0 "
		    "or later is required)" : strerror(errno));
	buf_free(&b);
	return rc == 0 ? 0 : 1;
}
