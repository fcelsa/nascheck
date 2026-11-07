/* sysinfo.h - general facts about the machine, for the system page. */
#ifndef NASCHECK_SYSINFO_H
#define NASCHECK_SYSINFO_H

#include <stdint.h>

struct sysinfo {
	char		host[64];
	char		os[160];	/* FreeBSD 15.1-RELEASE-p6 */
	char		kernel[128];	/* kernel ident or version */
	char		arch[72];
	char		cpu[96];	/* model name, or "" */
	int		ncpu;
	int64_t		uptime;		/* seconds, -1 if unknown */
	double		load[3];	/* -1 if unknown */
	uint64_t	mem_total;	/* bytes, 0 if unknown */
	uint64_t	mem_avail;	/* free or reclaimable */
};

/* Fill si; fields that cannot be read stay empty or unknown. */
void	sysinfo_read(struct sysinfo *si);

#endif
