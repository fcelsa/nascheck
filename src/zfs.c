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

	/* yyjson_obj_foreach must not see NULL: it computes NULL + 1. */
	if (!yyjson_is_obj(kids = yyjson_obj_get(v, "vdevs")))
		return 0;	/* a leaf */
	vd->children = (uint16_t)yyjson_obj_size(kids);
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
	/* "Deflated" space: what raidz leaves after parity. */
	p->usable = json_int(root, "/def_space", p->size);
	if (yyjson_is_obj(top = yyjson_obj_get(root, "vdevs"))) {
		yyjson_obj_foreach(top, idx, max, key, v) {
			if (add_vdev(p, &cap, v, 0, SECTION_DATA) == -1)
				return -1;
		}
	}
	for (i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
		if (!yyjson_is_obj(top = yyjson_obj_get(pv, groups[i].key)))
			continue;
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
	if (max > 0) {
		yyjson_obj_foreach(pools, idx, max, key, pv) {
			if (rc == -1)
				break;
			rc = parse_pool(pv, &z->pools[z->n]);
			z->n++;	/* also on error, so zfs_free() releases it */
		}
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
 * "2 LABEL gpt/data0 ...". A provider belongs to the last DISK above it;
 * a label stands for the provider right above it (the partition).
 */
static int
geom_lookup(const char *conftxt, const char *name, char *disk, size_t dsize,
    char *dev, size_t vsize)
{
	char stack[8][ZFS_NAME], cls[16], prov[ZFS_NAME];
	int isdisk = 0;
	const char *line = conftxt, *next;
	unsigned depth;

	for (; line != NULL && *line != '\0'; line = next) {
		next = strchr(line, '\n');
		if (next != NULL)
			next++;
		if (sscanf(line, "%u %15s %63s", &depth, cls, prov) != 3 ||
		    depth >= 8)
			continue;
		memcpy(stack[depth], prov, sizeof(prov));
		if (depth == 0)
			isdisk = strcmp(cls, "DISK") == 0;
		if (strcmp(prov, name) != 0)
			continue;
		if (!isdisk)
			return -1;	/* md, not a physical disk */
		snprintf(disk, dsize, "/dev/%s", stack[0]);
		snprintf(dev, vsize, "/dev/%s", strcmp(cls, "LABEL") == 0 &&
		    depth > 0 ? stack[depth - 1] : prov);
		return 0;
	}
	return -1;
}

void
zfs_map_disks(struct buf *b, struct zfs *z)
{
	char *argv[] = { tool("NASCHECK_SYSCTL", "sysctl"), "-n",
	    "kern.geom.conftxt", NULL };
	char real[PATH_MAX], disk[ZFS_NAME + 8], dev[ZFS_NAME + 8];
	char *conftxt = NULL;
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
			vd->disk[0] = vd->dev[0] = '\0';
			/* Only leaves on a device; not files. */
			if (strcmp(vd->type, "disk") != 0 ||
			    strncmp(vd->path, "/dev/", 5) != 0)
				continue;
			if (conftxt != NULL) {
				found = geom_lookup(conftxt, vd->path + 5, disk,
				    sizeof(disk), dev, sizeof(dev)) == 0;
			} else if ((found = realpath(vd->path, real) != NULL)) {
				/* Linux: /dev/disk/by-id/... -> /dev/sdb1. */
				snprintf(dev, sizeof(dev), "%.*s", ZFS_NAME, real);
				base = strrchr(real, '/');
				linux_disk(base != NULL ? base + 1 : real, disk,
				    sizeof(disk));
			}
			if (!found)
				continue;
			if (strlen(disk) < sizeof(vd->disk))
				memcpy(vd->disk, disk, strlen(disk) + 1);
			if (strlen(dev) < sizeof(vd->dev))
				memcpy(vd->dev, dev, strlen(dev) + 1);
		}
	}
	free(conftxt);
}

int
zfs_datasets(struct buf *b, const char *pool, struct dataset **out)
{
	char *argv[] = { tool("NASCHECK_ZFS", "zfs"), "list", "-H", "-p",
	    "-o", "name,used,avail,refer,mountpoint", "-t",
	    "filesystem,volume", "-r", (char *)pool, NULL };
	char *line, *next, *f[5];
	struct dataset *ds = NULL, *d;
	size_t n = 0, cap = 0;
	void *p;
	int code, k;

	*out = NULL;
	if (run_capture(argv, b, 1, &code) == -1)
		return -1;
	/* One dataset per line, five tab-separated fields; split in place. */
	for (line = b->data; line != NULL && *line != '\0'; line = next) {
		if ((next = strchr(line, '\n')) != NULL)
			*next++ = '\0';
		for (k = 0; k < 5; k++) {
			f[k] = line;
			if (k < 4 && (line = strchr(line, '\t')) != NULL)
				*line++ = '\0';
			if (line == NULL)
				break;
		}
		if (k < 4)
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 32;
			if ((p = realloc(ds, cap * sizeof(*ds))) == NULL) {
				free(ds);
				return -1;
			}
			ds = p;
		}
		d = &ds[n++];
		d->name = f[0];
		d->used = strtoll(f[1], NULL, 10);
		d->avail = strtoll(f[2], NULL, 10);
		d->refer = strtoll(f[3], NULL, 10);
		d->mountpoint = f[4];
	}
	*out = ds;
	return (int)n;
}

int
zfs_snapshots(struct buf *b, struct snapshots **out)
{
	char *argv[] = { tool("NASCHECK_ZFS", "zfs"), "list", "-H", "-p",
	    "-o", "name,creation", "-t", "snapshot", NULL };
	char *line, *next, *at, *t;
	struct snapshots *sn = NULL;
	size_t n = 0, cap = 0, i;
	int64_t created;
	void *p;
	int code;

	*out = NULL;
	if (run_capture(argv, b, 1, &code) == -1 || code != 0)
		return -1;
	/* "tank/home@daily-2026-11-08<TAB>1794614400": snapshots of one
	 * dataset usually follow each other, so look at the last one first. */
	for (line = b->data; line != NULL && *line != '\0'; line = next) {
		if ((next = strchr(line, '\n')) != NULL)
			*next++ = '\0';
		if ((at = strchr(line, '@')) == NULL ||
		    (t = strchr(at, '\t')) == NULL)
			continue;
		*at = '\0';
		created = strtoll(t + 1, NULL, 10);
		for (i = n; i > 0 && strcmp(sn[i - 1].dataset, line) != 0; i--)
			;
		if (i == 0) {
			if (n == cap) {
				cap = cap ? cap * 2 : 32;
				if ((p = realloc(sn, cap * sizeof(*sn))) == NULL) {
					free(sn);
					return -1;
				}
				sn = p;
			}
			sn[n].dataset = line;
			sn[n].newest = created;
			sn[n++].count = 0;
			i = n;
		}
		sn[i - 1].count++;
		if (created > sn[i - 1].newest)
			sn[i - 1].newest = created;
	}
	*out = sn;
	return (int)n;
}

void
zfs_layout_note(const struct vdev *v, size_t ntop, char *buf, size_t size)
{
	int parity, n = v->children;

	if (v->section == SECTION_LOGS)
		snprintf(buf, size, "intent log (SLOG): speeds up synchronous "
		    "writes");
	else if (v->section == SECTION_CACHE)
		snprintf(buf, size, "read cache (L2ARC): losing it loses no "
		    "data");
	else if (v->section == SECTION_SPARES)
		snprintf(buf, size, "hot spare: takes the place of a failed "
		    "disk");
	else if (strcmp(v->type, "mirror") == 0)
		snprintf(buf, size, "%d-way mirror: full copy on each disk, "
		    "survives %d failure%s", n, n - 1, n - 1 == 1 ? "" : "s");
	else if (strcmp(v->type, "raidz") == 0) {
		/* The parity level is in the name: raidz2-0. */
		parity = isdigit((unsigned char)v->name[5]) ?
		    v->name[5] - '0' : 1;
		snprintf(buf, size, "%d disks, %d parity: survives %d failure%s, "
		    "%d/%d of raw space usable", n, parity, parity,
		    parity == 1 ? "" : "s", n - parity, n);
	} else if (strncmp(v->type, "draid", 5) == 0)
		snprintf(buf, size, "dRAID: distributed parity and spare space");
	else
		snprintf(buf, size, "single disk, no redundancy: damage is "
		    "found, not repaired%s", ntop > 1 ? "; losing it loses the "
		    "pool" : "");
}
