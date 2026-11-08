/* sysinfo.h - general facts about the machine, for the system page. */
#ifndef NASCHECK_SYSINFO_H
#define NASCHECK_SYSINFO_H

#include <stdint.h>

#define SYSINFO_MAX_FS	8

/* A local filesystem that is not ZFS (ZFS pools are shown apart). */
struct fsinfo {
	char		mount[128];
	char		type[32];
	uint64_t	size;		/* bytes */
	uint64_t	used;
};

struct sysinfo {
	char		user[32];
	char		host[64];
	char		ip[48];		/* first IPv4 address, or "" */
	char		iface[16];	/* and its interface */
	char		term[64];	/* $TERM and the terminal program */
	char		locale[48];
	char		shell[32];
	char		os[160];	/* FreeBSD 15.1-RELEASE-p6, Ubuntu 24.04 */
	char		kernel[128];
	char		arch[72];
	char		board[200];	/* motherboard or system model */
	char		cpu[96];
	char		gpu[128];
	int		ncpu;
	int64_t		uptime;		/* seconds, -1 if unknown */
	uint64_t	mem_total;	/* bytes, 0 if unknown */
	uint64_t	mem_avail;	/* free or reclaimable */
	uint64_t	swap_total;
	uint64_t	swap_used;
	struct fsinfo	fs[SYSINFO_MAX_FS];
	int		nfs;
};

/* Fill si; fields that cannot be read stay empty or unknown. */
void	sysinfo_read(struct sysinfo *si);

#endif
