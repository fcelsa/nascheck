/* smart.c - disk discovery and SMART data through smartctl's JSON output. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smart.h"
#include "yyjson.h"

/* smartctl exit status bits (see smartctl(8), "RETURN VALUES"). */
#define SC_CMDLINE	0x01	/* command line did not parse */
#define SC_OPEN		0x02	/* open failed, or disk in low-power mode */

/* The test suite points this at a fake smartctl. */
static const char *
smartctl(void)
{
	const char *p = getenv("NASCHECK_SMARTCTL");

	return p != NULL && *p != '\0' ? p : "smartctl";
}

/* Run smartctl with args and parse its JSON in place inside b. */
static yyjson_doc *
smartctl_json(struct buf *b, char *const argv[], int *code)
{
	yyjson_doc *doc;

	if (run_capture(argv, b, YYJSON_PADDING_SIZE, code) == -1)
		return NULL;
	doc = yyjson_read_opts(b->data, b->len, YYJSON_READ_INSITU, NULL, NULL);
	if (doc == NULL)
		errno = EPROTO;
	return doc;
}

/* JSON pointer helpers: missing or mistyped values yield the default. */

static void
get_str(yyjson_val *root, const char *ptr, char *dst, size_t size)
{
	const char *s = yyjson_get_str(yyjson_ptr_get(root, ptr));

	snprintf(dst, size, "%s", s != NULL ? s : "");
}

/* Integers; 128-bit counters arrive as reals and are clamped. */
static int64_t
get_int(yyjson_val *root, const char *ptr, int64_t def)
{
	yyjson_val *v = yyjson_ptr_get(root, ptr);
	double d;

	if (yyjson_is_uint(v))
		return yyjson_get_uint(v) > INT64_MAX ? INT64_MAX :
		    (int64_t)yyjson_get_uint(v);
	if (yyjson_is_sint(v))
		return yyjson_get_sint(v);
	if (yyjson_is_real(v)) {
		d = yyjson_get_real(v);
		return d >= 9.2e18 ? INT64_MAX : d < 0 ? def : (int64_t)d;
	}
	return def;
}

/* Booleans as 1/0, -1 when absent. */
static int8_t
get_bool(yyjson_val *root, const char *ptr)
{
	yyjson_val *v = yyjson_ptr_get(root, ptr);

	return yyjson_is_bool(v) ? (int8_t)yyjson_get_bool(v) : -1;
}

static uint8_t
clamp_u8(int64_t v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/*
 * smartctl --scan also lists optical, tape, changer and enclosure devices
 * (FreeBSD CAM reports every peripheral). With -n standby they look like a
 * sleeping disk and cannot be told apart without waking the disks, so they
 * are recognized by driver name.
 */
static int
is_disk_node(const char *path)
{
	static const char *const skip[] = {
		"cd", "sa", "nsa", "esa", "ch", "ses",	/* FreeBSD */
		"sr", "st", "nst",			/* Linux */
	};
	const char *base = strrchr(path, '/');
	size_t i, len;

	base = base != NULL ? base + 1 : path;
	for (i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
		len = strlen(skip[i]);
		/* Match the driver name followed by the unit number only. */
		if (strncmp(base, skip[i], len) == 0 &&
		    base[len] >= '0' && base[len] <= '9')
			return 0;
	}
	return 1;
}

int
smart_scan(struct buf *b, struct disk **disks, size_t *n)
{
	char *argv[] = { (char *)smartctl(), "--scan", "-j", NULL };
	yyjson_doc *doc;
	yyjson_val *devs, *dev;
	struct disk *d;
	size_t i, idx, max;
	int code;

	*disks = NULL;
	*n = 0;
	if ((doc = smartctl_json(b, argv, &code)) == NULL)
		return -1;

	/* No "devices" key means no disk was found: not an error. */
	devs = yyjson_obj_get(yyjson_doc_get_root(doc), "devices");
	if ((max = yyjson_arr_size(devs)) > 0 &&
	    (*disks = calloc(max, sizeof(**disks))) == NULL) {
		yyjson_doc_free(doc);
		return -1;
	}
	yyjson_arr_foreach(devs, idx, max, dev) {
		const char *name = yyjson_get_str(yyjson_obj_get(dev, "name"));

		if (name == NULL || strlen(name) >= sizeof(d->path) ||
		    !is_disk_node(name))
			continue;
		/* Skip duplicates: the same node may be reached twice. */
		for (i = 0; i < *n && strcmp((*disks)[i].path, name) != 0; i++)
			;
		if (i < *n)
			continue;
		d = &(*disks)[(*n)++];
		memcpy(d->path, name, strlen(name) + 1);
	}
	yyjson_doc_free(doc);
	return 0;
}

static void
parse_ata(yyjson_val *root, struct disk *d)
{
	yyjson_val *table, *a, *t;
	struct ata_attr *at;
	const char *wf;
	size_t idx, max;

	table = yyjson_ptr_get(root, "/ata_smart_attributes/table");
	yyjson_arr_foreach(table, idx, max, a) {
		if (d->u.ata.nattr == ATA_MAX_ATTRS)
			break;
		at = &d->u.ata.attr[d->u.ata.nattr++];
		at->id = clamp_u8(get_int(a, "/id", 0));
		at->value = clamp_u8(get_int(a, "/value", 0));
		at->worst = clamp_u8(get_int(a, "/worst", 0));
		at->thresh = clamp_u8(get_int(a, "/thresh", 0));
		at->raw = get_int(a, "/raw/value", 0);
		get_str(a, "/name", at->name, sizeof(at->name));
		wf = yyjson_get_str(yyjson_obj_get(a, "when_failed"));
		at->failed = wf == NULL ? 0 : strcmp(wf, "now") == 0 ? 2 :
		    strcmp(wf, "past") == 0 ? 1 : 0;
		at->prefail = get_bool(a, "/flags/prefailure") == 1;
	}

	/* The newest self-test is the first entry of the log. */
	t = yyjson_ptr_get(root, "/ata_smart_self_test_log/standard/table/0");
	if (yyjson_is_obj(t)) {
		d->last_test.valid = 1;
		get_str(t, "/type/string", d->last_test.type,
		    sizeof(d->last_test.type));
		d->last_test.passed = get_bool(t, "/status/passed");
		d->last_test.hours = get_int(t, "/lifetime_hours", -1);
	}
}

static void
parse_nvme(yyjson_val *root, struct disk *d)
{
	yyjson_val *h, *table, *t;
	struct nvme_health *nv = &d->u.nvme;
	size_t idx, max;
	int64_t res;

	h = yyjson_obj_get(root, "nvme_smart_health_information_log");
	nv->critical_warning = clamp_u8(get_int(h, "/critical_warning", 0));
	nv->available_spare = clamp_u8(get_int(h, "/available_spare", 0));
	nv->spare_threshold = clamp_u8(get_int(h, "/available_spare_threshold", 0));
	nv->percentage_used = clamp_u8(get_int(h, "/percentage_used", 0));
	nv->media_errors = get_int(h, "/media_errors", 0);
	nv->err_log_entries = get_int(h, "/num_err_log_entries", 0);
	nv->unsafe_shutdowns = get_int(h, "/unsafe_shutdowns", 0);
	nv->data_units_written = get_int(h, "/data_units_written", 0);
	if (d->power_on_hours < 0)
		d->power_on_hours = get_int(h, "/power_on_hours", -1);
	if (d->temp == TEMP_UNKNOWN)
		d->temp = (int16_t)get_int(h, "/temperature", TEMP_UNKNOWN);

	/* The log may have holes (null entries): take the first real one. */
	table = yyjson_ptr_get(root, "/nvme_self_test_log/table");
	yyjson_arr_foreach(table, idx, max, t) {
		if (!yyjson_is_obj(t))
			continue;
		d->last_test.valid = 1;
		get_str(t, "/self_test_code/string", d->last_test.type,
		    sizeof(d->last_test.type));
		d->last_test.hours = get_int(t, "/power_on_hours", -1);
		/* 0 completed; 5-7 failed; the others were aborted. */
		res = get_int(t, "/self_test_result/value", -1);
		d->last_test.passed = res == 0 ? 1 :
		    (res >= 5 && res <= 7) ? 0 : -1;
		break;
	}
}

int
smart_read(struct buf *b, struct disk *d)
{
	char *argv[] = { (char *)smartctl(), "-a", "-j", "-n", "standby",
	    d->path, NULL };
	char path[sizeof(d->path)];
	const struct ata_attr *a;
	const char *proto, *pm;
	yyjson_doc *doc;
	yyjson_val *root;
	int code;

	/* Start from a clean record, keeping only the device path. */
	memcpy(path, d->path, sizeof(path));
	memset(d, 0, sizeof(*d));
	memcpy(d->path, path, sizeof(path));
	d->power_on_hours = -1;
	d->rotation_rate = -1;
	d->temp = d->temp_warn = d->temp_crit = TEMP_UNKNOWN;
	d->smart_passed = -1;
	d->last_test.passed = -1;

	if ((doc = smartctl_json(b, argv, &code)) == NULL)
		return -1;
	root = yyjson_doc_get_root(doc);

	/* smartctl reports a non-zero status also for healthy disks. */
	d->exit_status = clamp_u8(get_int(root, "/smartctl/exit_status", code));
	proto = yyjson_get_str(yyjson_ptr_get(root, "/device/protocol"));
	d->proto = proto == NULL ? PROTO_OTHER :
	    strcmp(proto, "ATA") == 0 ? PROTO_ATA :
	    strcmp(proto, "NVMe") == 0 ? PROTO_NVME : PROTO_OTHER;

	if (d->exit_status & (SC_CMDLINE | SC_OPEN)) {
		/* -n standby makes smartctl stop early on a sleeping disk. */
		pm = yyjson_get_str(yyjson_ptr_get(root, "/power_mode/name"));
		if (pm != NULL && (strncmp(pm, "STANDBY", 7) == 0 ||
		    strcmp(pm, "SLEEP") == 0)) {
			d->state = READ_STANDBY;
		} else {
			d->state = READ_ERROR;
			get_str(root, "/smartctl/messages/0/string", d->error,
			    sizeof(d->error));
		}
		yyjson_doc_free(doc);
		return 0;
	}

	d->state = READ_OK;
	get_str(root, "/model_name", d->model, sizeof(d->model));
	get_str(root, "/serial_number", d->serial, sizeof(d->serial));
	get_str(root, "/firmware_version", d->firmware, sizeof(d->firmware));
	d->capacity = (uint64_t)get_int(root, "/user_capacity/bytes",
	    get_int(root, "/nvme_total_capacity", 0));
	d->power_on_hours = get_int(root, "/power_on_time/hours", -1);
	d->temp = (int16_t)get_int(root, "/temperature/current", TEMP_UNKNOWN);
	/* NVMe: WCTEMP and CCTEMP; ATA disks rarely report them. */
	d->temp_warn = (int16_t)get_int(root, "/temperature/op_limit_max",
	    TEMP_UNKNOWN);
	d->temp_crit = (int16_t)get_int(root, "/temperature/critical_limit_max",
	    TEMP_UNKNOWN);
	d->smart_passed = get_bool(root, "/smart_status/passed");

	if (d->proto == PROTO_ATA) {
		d->rotation_rate = (int32_t)get_int(root, "/rotation_rate", -1);
		parse_ata(root, d);
		/*
		 * Disks older than ACS-2 do not report the rotation rate;
		 * a measured spin-up time still tells a spinning disk.
		 */
		if (d->rotation_rate < 0 &&
		    (a = smart_ata_attr(d, ATTR_SPIN_UP_TIME)) != NULL &&
		    a->raw > 0)
			d->rotation_rate = ROTATION_SPINNING;
	} else if (d->proto == PROTO_NVME) {
		d->rotation_rate = 0;
		parse_nvme(root, d);
	}
	yyjson_doc_free(doc);
	return 0;
}

const struct ata_attr *
smart_ata_attr(const struct disk *d, uint8_t id)
{
	uint8_t i;

	if (d->proto != PROTO_ATA)
		return NULL;
	for (i = 0; i < d->u.ata.nattr; i++)
		if (d->u.ata.attr[i].id == id)
			return &d->u.ata.attr[i];
	return NULL;
}
