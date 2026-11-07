/* nascheck - disk health monitor for FreeBSD and Linux. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "daemon.h"
#include "data.h"
#include "tui.h"

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
	    "usage: nascheck [-hlqV] [-c config] [command [device ...]]\n"
	    "\n"
	    "Without a command, nascheck opens its terminal interface.\n"
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
	    "  -l         read the disks, not the daemon's database (root)\n"
	    "  -q         check: print the summary line only\n");
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

static void
print_header(void)
{
	printf("%-8s %-6s %-4s %-24s %-20s %6s %5s %7s %-7s %6s %s\n",
	    "DEVICE", "HEALTH", "TYPE", "MODEL", "SERIAL", "SIZE", "TEMP",
	    "HOURS", "SMART", "TEST", "NOTES");
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

/* Short summary of a pool: size, use and last scan. */
static void
pool_notes(const struct pool *p, char *buf, size_t size)
{
	char total[16], day[16];
	struct tm tm;
	time_t end = (time_t)p->scan_end;
	size_t len;

	snprintf(buf, size, "%s, %lld%% used", fmt_size((uint64_t)p->size,
	    total, sizeof(total)), p->size > 0 ?
	    (long long)(p->alloc * 100 / p->size) : 0LL);
	len = strlen(buf);
	if (strcmp(p->scan_state, "SCANNING") == 0) {
		snprintf(buf + len, size - len, ", %s running",
		    strcmp(p->scan_func, "SCRUB") == 0 ? "scrub" : "resilver");
	} else if (p->scan_func[0] != '\0' && end > 0) {
		localtime_r(&end, &tm);
		strftime(day, sizeof(day), "%Y-%m-%d", &tm);
		snprintf(buf + len, size - len, ", %s %s",
		    strcmp(p->scan_func, "SCRUB") == 0 ? "scrub" : "resilver",
		    day);
	} else {
		snprintf(buf + len, size - len, ", never scrubbed");
	}
}

/* zpool status layout, with the disk under each leaf and its health. */
static void
print_pools(const struct set *set)
{
	static const char *const sections[] = { "", "logs", "cache", "spares" };
	const struct pool *p;
	const struct vdev *v;
	const char *disk, *health;
	char name[96], notes[160];
	enum vdev_section section;
	size_t i, j, k;

	printf("\n%-30s %-8s %5s %5s %5s %-6s %-6s %s\n", "POOL / VDEV",
	    "STATE", "READ", "WRITE", "CKSUM", "DISK", "HEALTH", "NOTES");
	for (i = 0; i < set->zfs.n; i++) {
		p = &set->zfs.pools[i];
		pool_notes(p, notes, sizeof(notes));
		printf("%-30.30s %-8s %5s %5s %5s %-6s %-6s %s\n", p->name,
		    p->state, "", "", "", "", level_word(set->ph[i].level),
		    notes);
		section = SECTION_DATA;
		for (j = 0; j < p->nvdev; j++) {
			v = &p->vdevs[j];
			/* Section headers line up with the pool, as in zpool. */
			if (v->section != section) {
				section = v->section;
				printf("%s\n", sections[section]);
			}
			/* The disk under a leaf, with its own health. */
			disk = health = "";
			for (k = 0; v->disk[0] != '\0' && k < set->n; k++) {
				if (strcmp(set->disks[k].path, v->disk) != 0)
					continue;
				disk = short_name(&set->disks[k]);
				health = level_short(&set->disks[k],
				    &set->dh[k]);
				break;
			}
			snprintf(name, sizeof(name), "%*s%s",
			    2 * (v->depth + 1), "", v->name);
			snprintf(notes, sizeof(notes), "%-30.30s %-8s %5lld "
			    "%5lld %5lld %-6s %s", name, v->state,
			    (long long)v->read_errors,
			    (long long)v->write_errors,
			    (long long)v->checksum_errors, disk, health);
			for (k = strlen(notes); k > 0 && notes[k - 1] == ' ';
			    k--)
				notes[k - 1] = '\0';
			puts(notes);
		}
	}
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
	time_t now = time(NULL);
	long silence;
	size_t i;
	int rc;

	if ((rc = get_set(c, b, names, count, live, &set)) != 0 ||
	    (rc = evaluate(c, &set, now)) != 0) {
		set_free(&set);
		return rc == 2 ? 1 : rc;
	}
	if ((silence = daemon_silence(c, &set, now)) >= 0)
		fprintf(stderr, "nascheck: no update from the daemon for "
		    "%ld min\n", silence);
	print_header();
	for (i = 0; i < set.n; i++)
		print_disk(&set.disks[i], &set.dh[i]);
	if (set.zfs.n > 0)
		print_pools(&set);
	rc = set.partial;
	set_free(&set);
	return rc;
}

/*
 * Summary line first, as monitoring systems read it, then one line per
 * finding. Returns the exit code: the worst level, or EXIT_UNKNOWN.
 */
/* One line per finding of a disk or pool. */
static void
print_findings(const char *subject, const struct health *h)
{
	uint8_t j;

	for (j = 0; j < h->n; j++)
		printf("%-8s %-8s %s\n", subject, level_name(h->f[j].level),
		    h->f[j].text);
}

static int
cmd_check(const struct config *c, struct buf *b, char **names, size_t count,
    int live, int quiet)
{
	struct set set;
	time_t now = time(NULL);
	size_t i, nwarn = 0, ncrit = 0;
	enum level l, worst = LEVEL_OK;
	long silence;
	int rc, code;

	if ((rc = get_set(c, b, names, count, live, &set)) != 0 ||
	    (rc = evaluate(c, &set, now)) != 0) {
		set_free(&set);
		return rc == 2 ? EXIT_UNKNOWN : rc;
	}
	for (i = 0; i < set.n + set.zfs.n; i++) {
		l = i < set.n ? set.dh[i].level : set.ph[i - set.n].level;
		nwarn += l == LEVEL_WARN;
		ncrit += l == LEVEL_CRIT;
		if (l > worst)
			worst = l;
	}

	/* Missing data makes the result unknown, unless already critical. */
	silence = daemon_silence(c, &set, now);
	code = worst == LEVEL_CRIT || (!set.partial && silence < 0) ?
	    (int)worst : EXIT_UNKNOWN;
	printf("%s: %zu disks, %zu pools, %zu critical, %zu warning%s\n",
	    code == EXIT_UNKNOWN ? "UNKNOWN" : level_name(worst), set.n,
	    set.zfs.n, ncrit, nwarn,
	    set.partial ? ", some data could not be read" : "");
	if (!quiet) {
		if (silence >= 0)
			printf("%-8s %-8s no update from the daemon for "
			    "%ld min\n", "-", "UNKNOWN", silence);
		for (i = 0; i < set.n; i++)
			print_findings(short_name(&set.disks[i]), &set.dh[i]);
		for (i = 0; i < set.zfs.n; i++)
			print_findings(set.zfs.pools[i].name, &set.ph[i]);
	}
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
	cmd = argc > 0 ? argv[0] : "tui";

	if (strcmp(cmd, "scan") == 0 && argc <= 1) {
		rc = cmd_scan(&b);
		goto out;
	}
	if (strcmp(cmd, "status") != 0 && strcmp(cmd, "check") != 0 &&
	    ((strcmp(cmd, "daemon") != 0 && strcmp(cmd, "refresh") != 0 &&
	    strcmp(cmd, "tui") != 0) || argc > 1)) {
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

	if (strcmp(cmd, "tui") == 0)
		return tui_run(&cfg, live);
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
