/* nascheck - disk health monitor for FreeBSD and Linux. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smart.h"

#ifndef NASCHECK_VERSION
#define NASCHECK_VERSION "dev"
#endif

static void
usage(FILE *f)
{
	fprintf(f,
	    "usage: nascheck [-hV] command [device ...]\n"
	    "\n"
	    "commands:\n"
	    "  scan               list the devices reported by smartctl\n"
	    "  status [device]    show the health of all or the given disks\n");
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

static void
print_header(void)
{
	printf("%-8s %-4s %-24s %-20s %6s %5s %7s %-7s %-4s %s\n",
	    "DEVICE", "TYPE", "MODEL", "SERIAL", "SIZE", "TEMP", "HOURS",
	    "SMART", "TEST", "NOTES");
}

static void
print_disk(const struct disk *d)
{
	const char *name = d->path, *smart;
	char size[16], temp[8], hours[24], notes[128], line[256], *end;

	if (strncmp(name, "/dev/", 5) == 0)
		name += 5;
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
	smart = d->state == READ_STANDBY ? "standby" :
	    d->state == READ_ERROR ? "error" :
	    tri(d->smart_passed, "PASSED", "FAILED");
	fmt_notes(d, notes, sizeof(notes));

	snprintf(line, sizeof(line),
	    "%-8s %-4s %-24.24s %-20.20s %6s %5s %7s %-7s %-4s %s",
	    name, disk_kind(d), d->model[0] ? d->model : "-",
	    d->serial[0] ? d->serial : "-",
	    d->state == READ_OK ? size : "-", temp, hours, smart,
	    d->last_test.valid ? tri(d->last_test.passed, "ok", "FAIL") : "-",
	    notes);
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
			return 1;
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
cmd_status(struct buf *b, char **names, size_t count)
{
	struct disk *disks;
	size_t i, n;
	int rc;

	if ((rc = get_disks(b, names, count, &disks, &n)) != 0)
		return rc;
	print_header();
	for (i = 0; i < n; i++) {
		if (smart_read(b, &disks[i]) == -1) {
			/* Without smartctl there is nothing more to try. */
			if (errno == ENOENT) {
				rc = -1;
				break;
			}
			fprintf(stderr, "nascheck: %s: %s\n", disks[i].path,
			    errno == EPROTO ? "smartctl output is not JSON" :
			    strerror(errno));
			rc = 1;
			continue;
		}
		/* Scanned devices we cannot judge (USB, SCSI) stay hidden. */
		if (count == 0 && disks[i].state == READ_OK &&
		    disks[i].proto == PROTO_OTHER)
			continue;
		print_disk(&disks[i]);
	}
	free(disks);
	return rc;
}

int
main(int argc, char **argv)
{
	struct buf b = { 0 };
	const char *cmd;
	int ch, rc;

	while ((ch = getopt(argc, argv, "hV")) != -1) {
		switch (ch) {
		case 'h':
			usage(stdout);
			return 0;
		case 'V':
			printf("nascheck %s\n", NASCHECK_VERSION);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc == 0) {
		usage(stderr);
		return 2;
	}

	cmd = argv[0];
	if (strcmp(cmd, "scan") == 0 && argc == 1)
		rc = cmd_scan(&b);
	else if (strcmp(cmd, "status") == 0)
		rc = cmd_status(&b, argv + 1, (size_t)argc - 1);
	else {
		usage(stderr);
		return 2;
	}
	if (rc == -1)
		fprintf(stderr, "nascheck: cannot use smartctl: %s\n",
		    errno == EPROTO ? "output is not JSON (smartmontools 7.0 "
		    "or later is required)" : strerror(errno));
	buf_free(&b);
	return rc == 0 ? 0 : 1;
}
