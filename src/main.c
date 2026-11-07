/* nascheck - disk health monitor for FreeBSD and Linux. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
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
	    "usage: nascheck [-hqV] [-c config] command [device ...]\n"
	    "\n"
	    "commands:\n"
	    "  scan               list the devices reported by smartctl\n"
	    "  status [device]    table of all or the given disks\n"
	    "  check [device]     report problems; exit 0 ok, 1 warning,\n"
	    "                     2 critical, 3 unknown\n"
	    "\n"
	    "  -c config  configuration file (default " NASCHECK_CONF ")\n"
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
 * Returns 0; 1 when some disk could not be read; 2 on a bad device name,
 * with nothing allocated (both already reported); -1 on a fatal error
 * left in errno.
 */
static int
read_disks(struct buf *b, char **names, size_t count, struct disk **disks,
    size_t *n)
{
	struct disk *d;
	size_t i, kept = 0;
	int rc;

	if ((rc = get_disks(b, names, count, disks, n)) != 0)
		return rc;
	for (i = 0; i < *n; i++) {
		d = &(*disks)[kept];
		if (i != kept)
			*d = (*disks)[i];
		if (smart_read(b, d) == -1) {
			/* Without smartctl there is nothing more to try. */
			if (errno == ENOENT) {
				free(*disks);
				*disks = NULL;
				return -1;
			}
			fprintf(stderr, "nascheck: %s: %s\n", d->path,
			    errno == EPROTO ? "smartctl output is not JSON" :
			    strerror(errno));
			rc = 1;
			continue;
		}
		if (count == 0 && d->state == READ_OK &&
		    d->proto == PROTO_OTHER)
			continue;
		kept++;
	}
	*n = kept;
	return rc;
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
cmd_status(const struct config *c, struct buf *b, char **names, size_t count)
{
	struct disk *disks;
	struct health h;
	size_t i, n;
	int rc;

	if ((rc = read_disks(b, names, count, &disks, &n)) == -1 || rc > 1)
		return rc;
	print_header();
	for (i = 0; i < n; i++) {
		health_eval(c, &disks[i], &h);
		print_disk(&disks[i], &h);
	}
	free(disks);
	return rc;
}

/*
 * Summary line first, as monitoring systems read it, then one line per
 * finding. Returns the exit code: the worst level, or EXIT_UNKNOWN.
 */
static int
cmd_check(const struct config *c, struct buf *b, char **names, size_t count,
    int quiet)
{
	struct disk *disks;
	struct health *h = NULL;
	size_t i, n, nwarn = 0, ncrit = 0;
	enum level worst = LEVEL_OK;
	uint8_t j;
	int rc, code;

	if ((rc = read_disks(b, names, count, &disks, &n)) == -1)
		return -1;
	if (rc > 1)
		return EXIT_UNKNOWN;
	if (n > 0 && (h = calloc(n, sizeof(*h))) == NULL) {
		free(disks);
		return -1;
	}
	for (i = 0; i < n; i++) {
		health_eval(c, &disks[i], &h[i]);
		nwarn += h[i].level == LEVEL_WARN;
		ncrit += h[i].level == LEVEL_CRIT;
		if (h[i].level > worst)
			worst = h[i].level;
	}
	/* An unreadable disk makes the result unknown, unless critical. */
	code = worst == LEVEL_CRIT || rc == 0 ? (int)worst : EXIT_UNKNOWN;
	printf("%s: %zu disks, %zu critical, %zu warning%s\n",
	    code == EXIT_UNKNOWN ? "UNKNOWN" : level_name(worst), n, ncrit,
	    nwarn, rc == 1 ? ", some disks could not be read" : "");
	for (i = 0; i < n && !quiet; i++)
		for (j = 0; j < h[i].n; j++)
			printf("%-8s %-8s %s\n", short_name(&disks[i]),
			    level_name(h[i].f[j].level), h[i].f[j].text);
	free(h);
	free(disks);
	return code;
}

int
main(int argc, char **argv)
{
	struct config cfg;
	struct buf b = { 0 };
	const char *cmd, *conf = NULL;
	int ch, quiet = 0, is_check, rc;

	while ((ch = getopt(argc, argv, "c:hqV")) != -1) {
		switch (ch) {
		case 'c':
			conf = optarg;
			break;
		case 'h':
			usage(stdout);
			return 0;
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
	is_check = strcmp(cmd, "check") == 0;
	if (!is_check && strcmp(cmd, "status") != 0 &&
	    (strcmp(cmd, "scan") != 0 || argc != 1)) {
		usage(stderr);
		return EXIT_USAGE;
	}

	if (strcmp(cmd, "scan") == 0) {
		rc = cmd_scan(&b);
	} else {
		/* The default file is optional, one given with -c is not. */
		config_defaults(&cfg);
		if (config_load(&cfg, conf != NULL ? conf : NASCHECK_CONF,
		    conf == NULL) == -1)
			return is_check ? EXIT_UNKNOWN : 1;
		if (is_check)
			rc = cmd_check(&cfg, &b, argv + 1, (size_t)argc - 1,
			    quiet);
		else
			rc = cmd_status(&cfg, &b, argv + 1, (size_t)argc - 1);
	}
	if (rc == -1)
		fprintf(stderr, "nascheck: cannot use smartctl: %s\n",
		    errno == EPROTO ? "output is not JSON (smartmontools 7.0 "
		    "or later is required)" : strerror(errno));
	buf_free(&b);
	if (is_check)
		return rc == -1 ? EXIT_UNKNOWN : rc;
	return rc == 0 ? 0 : 1;
}
