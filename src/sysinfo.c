/* sysinfo.c - general facts about the machine, for the system page. */

/*
 * The Makefile asks for strict POSIX; here we need the BSD and GNU
 * extensions (sysctl, getloadavg), so take the system's default view.
 */
#undef _XOPEN_SOURCE
#ifdef __linux__
#define _DEFAULT_SOURCE
#endif

#include <sys/types.h>
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#include <sys/time.h>
#endif
#include <sys/utsname.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sysinfo.h"

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

static void
platform(struct sysinfo *si)
{
	struct timeval boot;
	size_t len = sizeof(boot);
	uint64_t page = sysctl_u64("hw.pagesize");

	sysctl_str("hw.model", si->cpu, sizeof(si->cpu));
	si->mem_total = sysctl_u64("hw.physmem");
	/* Free and inactive pages can be handed out at once. */
	si->mem_avail = page * (sysctl_u64("vm.stats.vm.v_free_count") +
	    sysctl_u64("vm.stats.vm.v_inactive_count"));
	if (sysctlbyname("kern.boottime", &boot, &len, NULL, 0) == 0)
		si->uptime = (int64_t)(time(NULL) - boot.tv_sec);
}
#elif defined(__linux__)
/* The value after "key:" in a /proc file, or "". */
static void
proc_field(const char *file, const char *key, char *dst, size_t size)
{
	char line[256], *p;
	size_t klen = strlen(key);
	FILE *f;

	dst[0] = '\0';
	if ((f = fopen(file, "r")) == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, key, klen) != 0 ||
		    (p = strchr(line + klen, ':')) == NULL)
			continue;
		for (p++; *p == ' ' || *p == '\t'; p++)
			;
		p[strcspn(p, "\n")] = '\0';
		snprintf(dst, size, "%s", p);
		break;
	}
	fclose(f);
}

static void
platform(struct sysinfo *si)
{
	char buf[64];
	double up;
	FILE *f;

	proc_field("/proc/cpuinfo", "model name", si->cpu, sizeof(si->cpu));
	proc_field("/proc/meminfo", "MemTotal", buf, sizeof(buf));
	si->mem_total = strtoull(buf, NULL, 10) * 1024;	/* kB */
	proc_field("/proc/meminfo", "MemAvailable", buf, sizeof(buf));
	si->mem_avail = strtoull(buf, NULL, 10) * 1024;
	if ((f = fopen("/proc/uptime", "r")) != NULL) {
		if (fscanf(f, "%lf", &up) == 1)
			si->uptime = (int64_t)up;
		fclose(f);
	}
}
#else
static void
platform(struct sysinfo *si)
{
	(void)si;
}
#endif

void
sysinfo_read(struct sysinfo *si)
{
	struct utsname u;
	long n;

	memset(si, 0, sizeof(*si));
	si->uptime = -1;
	si->load[0] = si->load[1] = si->load[2] = -1;
	if (gethostname(si->host, sizeof(si->host)) == -1)
		si->host[0] = '\0';
	si->host[sizeof(si->host) - 1] = '\0';
	if (uname(&u) == 0) {
		snprintf(si->os, sizeof(si->os), "%s %s", u.sysname,
		    u.release);
		snprintf(si->kernel, sizeof(si->kernel), "%s", u.version);
		si->kernel[strcspn(si->kernel, "\n")] = '\0';
		snprintf(si->arch, sizeof(si->arch), "%s", u.machine);
	}
	if ((n = sysconf(_SC_NPROCESSORS_ONLN)) > 0)
		si->ncpu = (int)n;
	if (getloadavg(si->load, 3) != 3)
		si->load[0] = si->load[1] = si->load[2] = -1;
	platform(si);
}
