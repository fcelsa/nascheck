/* sysinfo.c - general facts about the machine, for the system page. */

/*
 * The Makefile asks for strict POSIX; here we need the BSD and GNU
 * extensions (sysctl, kenv, getifaddrs, getmntinfo), so take the
 * system's default view.
 */
#undef _XOPEN_SOURCE
#ifdef __linux__
#define _DEFAULT_SOURCE
#endif

#include <sys/types.h>
#include <sys/socket.h>
#ifdef __FreeBSD__
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/ucred.h>
#include <kenv.h>
#elif defined(__linux__)
#include <sys/statvfs.h>
#endif
#include <sys/utsname.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

#include <ifaddrs.h>
#include <locale.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "run.h"
#include "sysinfo.h"

/* Trailing blanks and newlines off. */
static void
chomp(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == ' ' ||
	    s[n - 1] == '\t'))
		s[--n] = '\0';
}

static void
add_fs(struct sysinfo *si, const char *mount, const char *type,
    uint64_t size, uint64_t avail)
{
	struct fsinfo *f;

	if (si->nfs == SYSINFO_MAX_FS || size == 0)
		return;
	f = &si->fs[si->nfs++];
	snprintf(f->mount, sizeof(f->mount), "%s", mount);
	snprintf(f->type, sizeof(f->type), "%s", type);
	f->size = size;
	f->used = size - (avail < size ? avail : size);
}

#ifdef __FreeBSD__
static void
sysctl_str(const char *name, char *dst, size_t size)
{
	size_t len = size;

	if (sysctlbyname(name, dst, &len, NULL, 0) == -1)
		dst[0] = '\0';
	else
		dst[size - 1] = '\0';
}

static uint64_t
sysctl_u64(const char *name)
{
	uint64_t v64 = 0;
	unsigned int v32;
	size_t len = sizeof(v64);

	/* Some counters are 32-bit: the length tells which. */
	if (sysctlbyname(name, &v64, &len, NULL, 0) == -1)
		return 0;
	if (len == sizeof(v32)) {
		memcpy(&v32, &v64, sizeof(v32));
		return v32;
	}
	return v64;
}

/* "maker product" from two SMBIOS variables of the kernel environment. */
static int
smbios(const char *maker, const char *product, char *dst, size_t size)
{
	char m[64], p[64];

	if (kenv(KENV_GET, maker, m, sizeof(m)) <= 0 ||
	    kenv(KENV_GET, product, p, sizeof(p)) <= 0)
		return -1;
	snprintf(dst, size, "%s %s", m, p);
	return 0;
}

/* The first display device of `pciconf -lv`: vendor and device names. */
static void
gpu(char *dst, size_t size)
{
	char *argv[] = { "/usr/sbin/pciconf", "-lv", NULL };
	char vendor[64] = "", device[96] = "", *line, *next, *q;
	struct buf b = { 0 };
	int code;

	if (run_capture(argv, &b, 1, &code) == -1 || code != 0)
		goto out;
	for (line = b.data; line != NULL; line = next) {
		if ((next = strchr(line, '\n')) != NULL)
			*next++ = '\0';
		if (line[0] != ' ' && line[0] != '\t') {
			vendor[0] = device[0] = '\0';	/* a new device */
			continue;
		}
		if ((q = strchr(line, '\'')) != NULL) {
			if (strstr(line, "vendor") != NULL)
				snprintf(vendor, sizeof(vendor), "%s", q + 1);
			else if (strstr(line, "device") != NULL)
				snprintf(device, sizeof(device), "%s", q + 1);
			vendor[strcspn(vendor, "'")] = '\0';
			device[strcspn(device, "'")] = '\0';
		}
		if (strstr(line, "class") != NULL &&
		    strstr(line, "display") != NULL) {
			snprintf(dst, size, "%s %s", vendor, device);
			break;
		}
	}
out:
	buf_free(&b);
}

/* `swapinfo -k`: one line per device, in KiB. */
static void
swap(struct sysinfo *si)
{
	char *argv[] = { "/usr/sbin/swapinfo", "-k", NULL };
	char *line, *next;
	unsigned long long total, used;
	struct buf b = { 0 };
	int code;

	if (run_capture(argv, &b, 1, &code) == 0 && code == 0) {
		for (line = b.data; line != NULL; line = next) {
			if ((next = strchr(line, '\n')) != NULL)
				*next++ = '\0';
			if (strncmp(line, "/dev/", 5) == 0 && sscanf(line,
			    "%*s %llu %llu", &total, &used) == 2) {
				si->swap_total += total * 1024;
				si->swap_used += used * 1024;
			}
		}
	}
	buf_free(&b);
}

static void
platform(struct sysinfo *si)
{
	struct statfs *mnt;
	struct timeval boot;
	char ident[32];
	size_t len = sizeof(boot);
	uint64_t page = sysctl_u64("hw.pagesize");
	int i, n;

	sysctl_str("kern.ident", ident, sizeof(ident));
	if (ident[0] != '\0') {
		chomp(si->kernel);
		snprintf(si->kernel + strlen(si->kernel), sizeof(si->kernel) -
		    strlen(si->kernel), " (%s)", ident);
	}
	if (smbios("smbios.planar.maker", "smbios.planar.product",
	    si->board, sizeof(si->board)) == -1)
		smbios("smbios.system.maker", "smbios.system.product",
		    si->board, sizeof(si->board));
	sysctl_str("hw.model", si->cpu, sizeof(si->cpu));
	gpu(si->gpu, sizeof(si->gpu));
	si->mem_total = sysctl_u64("hw.physmem");
	/* Free and inactive pages can be handed out at once. */
	si->mem_avail = page * (sysctl_u64("vm.stats.vm.v_free_count") +
	    sysctl_u64("vm.stats.vm.v_inactive_count"));
	swap(si);
	if (sysctlbyname("kern.boottime", &boot, &len, NULL, 0) == 0)
		si->uptime = (int64_t)(time(NULL) - boot.tv_sec);
	/* Local filesystems other than ZFS. */
	n = getmntinfo(&mnt, MNT_NOWAIT);
	for (i = 0; i < n; i++)
		if (strcmp(mnt[i].f_fstypename, "ufs") == 0 ||
		    strcmp(mnt[i].f_fstypename, "msdosfs") == 0 ||
		    strcmp(mnt[i].f_fstypename, "ext2fs") == 0)
			add_fs(si, mnt[i].f_mntonname, mnt[i].f_fstypename,
			    mnt[i].f_blocks * mnt[i].f_bsize,
			    (uint64_t)mnt[i].f_bavail * mnt[i].f_bsize);
}
#elif defined(__linux__)
/* The first line of a small file, or "". */
static void
read_line(const char *file, char *dst, size_t size)
{
	FILE *f;

	dst[0] = '\0';
	if ((f = fopen(file, "r")) == NULL)
		return;
	if (fgets(dst, (int)size, f) == NULL)
		dst[0] = '\0';
	fclose(f);
	chomp(dst);
}

/* The value after "key" and a separator (':' or '=') in a file, or "". */
static void
file_field(const char *file, const char *key, char sep, char *dst,
    size_t size)
{
	char line[256], *p;
	size_t klen = strlen(key);
	FILE *f;

	dst[0] = '\0';
	if ((f = fopen(file, "r")) == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, key, klen) != 0 ||
		    (p = strchr(line + klen, sep)) == NULL)
			continue;
		for (p++; *p == ' ' || *p == '\t' || *p == '"'; p++)
			;
		chomp(p);
		p[strcspn(p, "\"")] = '\0';
		snprintf(dst, size, "%s", p);
		break;
	}
	fclose(f);
}

/* The name of PCI vendor:device from the pci.ids database. */
static void
pci_name(unsigned vendor, unsigned device, char *dst, size_t size)
{
	static const char *const dbs[] = {
		"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids",
		"/usr/share/pci.ids",
	};
	char line[256], vname[96] = "";
	unsigned id;
	size_t i, n, k;
	FILE *f = NULL;

	snprintf(dst, size, "PCI %04x:%04x", vendor, device);
	for (i = 0; i < sizeof(dbs) / sizeof(dbs[0]) && f == NULL; i++)
		f = fopen(dbs[i], "r");
	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		chomp(line);
		if (line[0] != '\t' && line[0] != '#' &&
		    sscanf(line, "%x", &id) == 1) {
			if (vname[0] != '\0')
				break;	/* past our vendor */
			if (id == vendor)
				snprintf(vname, sizeof(vname), "%.*s",
				    (int)sizeof(vname) - 1, line + 6);
		} else if (vname[0] != '\0' && line[0] == '\t' &&
		    line[1] != '\t' && sscanf(line + 1, "%x", &id) == 1 &&
		    id == device) {
			/* Vendor, then as much of the device name as fits. */
			n = (size_t)snprintf(dst, size, "%s ", vname);
			if (n < size) {
				k = strlen(line + 7);
				k = k < size - n - 1 ? k : size - n - 1;
				memcpy(dst + n, line + 7, k);
				dst[n + k] = '\0';
			}
			fclose(f);
			return;
		}
	}
	if (vname[0] != '\0')
		snprintf(dst, size, "%s %04x", vname, device);
	fclose(f);
}

/* The first display controller (PCI class 03) in sysfs. */
static void
gpu(char *dst, size_t size)
{
	char path[128], val[32];
	unsigned vendor, device;
	int i;

	for (i = 0; i < 64; i++) {
		snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/"
		    "class", i);
		read_line(path, val, sizeof(val));
		if (val[0] == '\0')
			continue;
		if (strncmp(val, "0x03", 4) != 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/"
		    "vendor", i);
		read_line(path, val, sizeof(val));
		vendor = (unsigned)strtoul(val, NULL, 16);
		snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/"
		    "device", i);
		read_line(path, val, sizeof(val));
		device = (unsigned)strtoul(val, NULL, 16);
		pci_name(vendor, device, dst, size);
		return;
	}
}

static void
platform(struct sysinfo *si)
{
	static const char *const types[] = {
		"ext2", "ext3", "ext4", "xfs", "btrfs", "f2fs", "vfat",
		"exfat", "jfs",
	};
	char buf[128], vendor[64], dev[128], mount[128], type[32];
	struct statvfs vs;
	double up;
	size_t i;
	FILE *f;

	file_field("/etc/os-release", "PRETTY_NAME", '=', buf, sizeof(buf));
	if (buf[0] != '\0')
		snprintf(si->os, sizeof(si->os), "%s", buf);
	read_line("/sys/class/dmi/id/board_vendor", vendor, sizeof(vendor));
	read_line("/sys/class/dmi/id/board_name", buf, sizeof(buf));
	if (buf[0] == '\0') {
		read_line("/sys/class/dmi/id/sys_vendor", vendor,
		    sizeof(vendor));
		read_line("/sys/class/dmi/id/product_name", buf, sizeof(buf));
	}
	if (buf[0] != '\0')
		snprintf(si->board, sizeof(si->board), "%s %s", vendor, buf);
	file_field("/proc/cpuinfo", "model name", ':', si->cpu,
	    sizeof(si->cpu));
	gpu(si->gpu, sizeof(si->gpu));
	file_field("/proc/meminfo", "MemTotal", ':', buf, sizeof(buf));
	si->mem_total = strtoull(buf, NULL, 10) * 1024;	/* kB */
	file_field("/proc/meminfo", "MemAvailable", ':', buf, sizeof(buf));
	si->mem_avail = strtoull(buf, NULL, 10) * 1024;
	file_field("/proc/meminfo", "SwapTotal", ':', buf, sizeof(buf));
	si->swap_total = strtoull(buf, NULL, 10) * 1024;
	file_field("/proc/meminfo", "SwapFree", ':', buf, sizeof(buf));
	si->swap_used = si->swap_total - strtoull(buf, NULL, 10) * 1024;
	if ((f = fopen("/proc/uptime", "r")) != NULL) {
		if (fscanf(f, "%lf", &up) == 1)
			si->uptime = (int64_t)up;
		fclose(f);
	}
	/* Local filesystems other than ZFS, on a device. */
	if ((f = fopen("/proc/mounts", "r")) == NULL)
		return;
	while (fscanf(f, "%127s %127s %31s %*[^\n]", dev, mount, type) == 3) {
		if (strncmp(dev, "/dev/", 5) != 0)
			continue;
		for (i = 0; i < sizeof(types) / sizeof(types[0]); i++)
			if (strcmp(type, types[i]) == 0)
				break;
		if (i < sizeof(types) / sizeof(types[0]) &&
		    statvfs(mount, &vs) == 0)
			add_fs(si, mount, type, (uint64_t)vs.f_blocks *
			    vs.f_frsize, (uint64_t)vs.f_bavail * vs.f_frsize);
	}
	fclose(f);
}
#else
static void
platform(struct sysinfo *si)
{
	(void)si;
}
#endif

/* The first IPv4 address of an interface that is up, not loopback. */
static void
local_ip(struct sysinfo *si)
{
	struct ifaddrs *ifs, *i;

	if (getifaddrs(&ifs) == -1)
		return;
	for (i = ifs; i != NULL; i = i->ifa_next) {
		if (i->ifa_addr == NULL || i->ifa_addr->sa_family != AF_INET ||
		    !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK))
			continue;
		inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)
		    i->ifa_addr)->sin_addr, si->ip, sizeof(si->ip));
		snprintf(si->iface, sizeof(si->iface), "%s", i->ifa_name);
		break;
	}
	freeifaddrs(ifs);
}

void
sysinfo_read(struct sysinfo *si)
{
	const char *term = getenv("TERM"), *prog = getenv("TERM_PROGRAM");
	const char *shell = getenv("SHELL"), *loc, *base;
	struct passwd *pw;
	struct utsname u;
	long n;

	memset(si, 0, sizeof(*si));
	si->uptime = -1;
	if (gethostname(si->host, sizeof(si->host)) == -1)
		si->host[0] = '\0';
	si->host[sizeof(si->host) - 1] = '\0';
	if ((pw = getpwuid(getuid())) != NULL) {
		snprintf(si->user, sizeof(si->user), "%s", pw->pw_name);
		if (shell == NULL || *shell == '\0')
			shell = pw->pw_shell;
	}
	if (shell != NULL && (base = strrchr(shell, '/')) != NULL)
		shell = base + 1;
	snprintf(si->shell, sizeof(si->shell), "%s", shell != NULL ? shell :
	    "");
	snprintf(si->term, sizeof(si->term), "%s%s%s%s", term != NULL ?
	    term : "", prog != NULL ? " (" : "", prog != NULL ? prog : "",
	    prog != NULL ? ")" : "");
	loc = setlocale(LC_CTYPE, NULL);
	snprintf(si->locale, sizeof(si->locale), "%s", loc != NULL ? loc : "");
	local_ip(si);
	if (uname(&u) == 0) {
		snprintf(si->os, sizeof(si->os), "%s %s", u.sysname,
		    u.release);
		snprintf(si->kernel, sizeof(si->kernel), "%s", u.release);
		snprintf(si->arch, sizeof(si->arch), "%s", u.machine);
	}
	if ((n = sysconf(_SC_NPROCESSORS_ONLN)) > 0)
		si->ncpu = (int)n;
	platform(si);
}
