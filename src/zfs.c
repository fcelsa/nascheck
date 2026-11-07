/* zfs.c - ZFS pools through `zpool status -j`, and their physical disks. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "zfs.h"

/* The test suite points these at fakes. */
static char *
tool(const char *env, const char *def)
{
	const char *p = getenv(env);

	return (char *)(p != NULL && *p != '\0' ? p : def);
}

void
zfs_free(struct zfs *z)
{
	size_t i;

	for (i = 0; i < z->n; i++)
		free(z->pools[i].vdevs);
	free(z->pools);
	z->pools = NULL;
	z->n = 0;
}

/* Append one vdev and its children, depth first. */
static int
add_vdev(struct pool *p, size_t *cap, yyjson_val *v, uint8_t depth,
    enum vdev_section section)
{
	yyjson_val *kids, *key, *kid;
	struct vdev *vd;
	size_t idx, max;
	void *np;

	if (p->nvdev == *cap) {
		*cap = *cap ? *cap * 2 : 16;
		if ((np = realloc(p->vdevs, *cap * sizeof(*vd))) == NULL)
			return -1;
		p->vdevs = np;
	}
	vd = &p->vdevs[p->nvdev++];
	memset(vd, 0, sizeof(*vd));
	json_str(v, "/name", vd->name, sizeof(vd->name));
	json_str(v, "/vdev_type", vd->type, sizeof(vd->type));
	json_str(v, "/state", vd->state, sizeof(vd->state));
	json_str(v, "/path", vd->path, sizeof(vd->path));
	vd->read_errors = json_int(v, "/read_errors", 0);
	vd->write_errors = json_int(v, "/write_errors", 0);
	vd->checksum_errors = json_int(v, "/checksum_errors", 0);
	vd->depth = depth;
	vd->section = section;

	kids = yyjson_obj_get(v, "vdevs");
	yyjson_obj_foreach(kids, idx, max, key, kid) {
		if (add_vdev(p, cap, kid, depth + 1, section) == -1)
			return -1;
	}
	return 0;
}

static int
parse_pool(yyjson_val *pv, struct pool *p)
{
	static const struct {
		const char		*key;
		enum vdev_section	 section;
	} groups[] = {
		{ "logs", SECTION_LOGS },
		{ "l2cache", SECTION_CACHE },
		{ "spares", SECTION_SPARES },
	};
	yyjson_val *root, *key, *v, *top;
	size_t idx, max, cap = 0, i;

	memset(p, 0, sizeof(*p));
	json_str(pv, "/name", p->name, sizeof(p->name));
	json_str(pv, "/state", p->state, sizeof(p->state));
	p->data_errors = json_int(pv, "/error_count", 0);
	json_str(pv, "/scan_stats/function", p->scan_func,
	    sizeof(p->scan_func));
	json_str(pv, "/scan_stats/state", p->scan_state,
	    sizeof(p->scan_state));
	p->scan_end = json_int(pv, "/scan_stats/end_time", 0);
	p->scan_errors = json_int(pv, "/scan_stats/errors", 0);

	/* "vdevs" holds the root vdev, named after the pool. */
	root = yyjson_obj_get(yyjson_obj_get(pv, "vdevs"), p->name);
	p->alloc = json_int(root, "/alloc_space", 0);
	p->size = json_int(root, "/total_space", 0);
	top = yyjson_obj_get(root, "vdevs");
	yyjson_obj_foreach(top, idx, max, key, v) {
		if (add_vdev(p, &cap, v, 0, SECTION_DATA) == -1)
			return -1;
	}
	for (i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
		top = yyjson_obj_get(pv, groups[i].key);
		yyjson_obj_foreach(top, idx, max, key, v) {
			if (add_vdev(p, &cap, v, 0, groups[i].section) == -1)
				return -1;
		}
	}
	return 0;
}

int
zfs_parse(const char *json, size_t len, struct zfs *z)
{
	yyjson_doc *doc;
	yyjson_val *pools, *key, *pv;
	size_t idx, max;
	int rc = 0;

	z->pools = NULL;
	z->n = 0;
	if ((doc = yyjson_read(json, len, 0)) == NULL) {
		errno = EPROTO;
		return -1;
	}
	pools = yyjson_obj_get(yyjson_doc_get_root(doc), "pools");
	if ((max = yyjson_obj_size(pools)) > 0 &&
	    (z->pools = calloc(max, sizeof(*z->pools))) == NULL)
		rc = -1;
	yyjson_obj_foreach(pools, idx, max, key, pv) {
		if (rc == -1)
			break;
		rc = parse_pool(pv, &z->pools[z->n]);
		z->n++;		/* also on error, so zfs_free() releases it */
	}
	yyjson_doc_free(doc);
	if (rc == -1)
		zfs_free(z);
	return rc;
}

int
zfs_read(struct buf *b, struct zfs *z)
{
	char *status[] = { tool("NASCHECK_ZPOOL", "zpool"), "status", "-j",
	    "--json-int", "-p", NULL };
	char *list[] = { status[0], "list", "-H", "-o", "name", NULL };
	int code;

	z->pools = NULL;
	z->n = 0;
	if (run_capture(status, b, 1, &code) == -1)
		return errno == ENOENT ? 0 : -1;	/* no ZFS at all */
	if (zfs_parse(b->data, b->len, z) == 0)
		return 0;
	/*
	 * No JSON: either there are no pools, or this zpool predates -j
	 * (OpenZFS 2.3). Tell them apart by listing the pools.
	 */
	if (run_capture(list, b, 1, &code) == -1 || code != 0 || b->len == 0)
		return 0;
	errno = EPROTO;
	return -1;
}

/*
 * Disk names from partition names on Linux: sdb1 -> sdb, nvme0n1p2 and
 * nvme0n1 -> nvme0 (smartctl talks to the NVMe controller).
 */
static void
linux_disk(const char *part, char *dst, size_t size)
{
	char name[ZFS_NAME], *p;

	if (strlen(part) >= sizeof(name)) {
		dst[0] = '\0';
		return;
	}
	memcpy(name, part, strlen(part) + 1);
	if (strncmp(name, "nvme", 4) == 0) {
		if ((p = strchr(name + 4, 'n')) != NULL)
			*p = '\0';
	} else {
		for (p = name + strlen(name); p > name &&
		    isdigit((unsigned char)p[-1]); p--)
			;
		*p = '\0';
	}
	snprintf(dst, size, "/dev/%s", name);
}

/*
 * FreeBSD: kern.geom.conftxt lists every GEOM provider, one per line,
 * indented by depth: "0 DISK ada0 ...", "1 PART ada0p1 ...",
 * "2 LABEL gpt/data0 ...". A provider belongs to the last DISK above it.
 */
static int
geom_disk(const char *conftxt, const char *name, char *dst, size_t size)
{
	char disk[ZFS_NAME] = "", cls[16], prov[ZFS_NAME];
	const char *line = conftxt, *next;
	unsigned depth;

	for (; line != NULL && *line != '\0'; line = next) {
		next = strchr(line, '\n');
		if (next != NULL)
			next++;
		if (sscanf(line, "%u %15s %63s", &depth, cls, prov) != 3)
			continue;
		if (depth == 0)
			snprintf(disk, sizeof(disk), "%s",
			    strcmp(cls, "DISK") == 0 ? prov : "");
		if (strcmp(prov, name) == 0) {
			if (disk[0] == '\0')
				return -1;	/* md, not a physical disk */
			snprintf(dst, size, "/dev/%s", disk);
			return 0;
		}
	}
	return -1;
}

void
zfs_map_disks(struct buf *b, struct zfs *z)
{
	char *argv[] = { tool("NASCHECK_SYSCTL", "sysctl"), "-n",
	    "kern.geom.conftxt", NULL };
	char real[PATH_MAX], *conftxt = NULL;
	const char *base;
	struct vdev *vd;
	size_t i, j;
	int code, found;

	/* The GEOM tree exists on FreeBSD only; keep a copy of it. */
	if (run_capture(argv, b, 1, &code) == 0 && code == 0 && b->len > 0)
		conftxt = strdup(b->data);

	for (i = 0; i < z->n; i++) {
		for (j = 0; j < z->pools[i].nvdev; j++) {
			vd = &z->pools[i].vdevs[j];
			vd->disk[0] = '\0';
			/* Only leaves on a device; not files. */
			if (strcmp(vd->type, "disk") != 0 ||
			    strncmp(vd->path, "/dev/", 5) != 0)
				continue;
			if (conftxt != NULL) {
				found = geom_disk(conftxt, vd->path + 5, real,
				    sizeof(real)) == 0;
			} else if ((found = realpath(vd->path, real) != NULL)) {
				base = strrchr(real, '/');
				linux_disk(base != NULL ? base + 1 : real, real,
				    sizeof(real));
			}
			if (found && strlen(real) < sizeof(vd->disk))
				memcpy(vd->disk, real, strlen(real) + 1);
		}
	}
	free(conftxt);
}
