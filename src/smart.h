/* smart.h - disk discovery and SMART data through smartctl's JSON output. */
#ifndef NASCHECK_SMART_H
#define NASCHECK_SMART_H

#include <stddef.h>
#include <stdint.h>

#include "run.h"

#define ATA_MAX_ATTRS	30	/* the ATA attribute table holds 30 entries */
#define TEMP_UNKNOWN	INT16_MIN
#define ROTATION_SPINNING 1	/* rotation_rate: spinning, rpm not reported */

/* ATA attributes used by nascheck. */
#define ATTR_SPIN_UP_TIME	3	/* only spinning disks measure it */
#define ATTR_REALLOC		5	/* Reallocated_Sector_Ct */
#define ATTR_PENDING		197	/* Current_Pending_Sector */
#define ATTR_OFFLINE		198	/* Offline_Uncorrectable */

enum proto {
	PROTO_OTHER,		/* SCSI, USB without SAT, unknown: not handled */
	PROTO_ATA,
	PROTO_NVME
};

/* Result of reading a device. */
enum read_state {
	READ_OK,		/* SMART data available */
	READ_STANDBY,		/* disk spun down, left asleep */
	READ_ERROR		/* smartctl could not read the disk, see error */
};

struct ata_attr {
	char	 name[32];
	int64_t	 raw;
	uint8_t	 id;
	uint8_t	 value;		/* normalized value, 0 if not reported */
	uint8_t	 worst;
	uint8_t	 thresh;
	uint8_t	 failed;	/* 0 never, 1 in the past, 2 now */
};

struct nvme_health {
	int64_t	 media_errors;
	int64_t	 err_log_entries;
	int64_t	 unsafe_shutdowns;
	int64_t	 data_units_written;	/* units of 512000 bytes */
	uint8_t	 critical_warning;	/* bit mask, 0 = no warning */
	uint8_t	 available_spare;	/* percent */
	uint8_t	 spare_threshold;	/* percent */
	uint8_t	 percentage_used;	/* wear estimate, may exceed 100 */
};

struct selftest {
	char	 type[24];	/* e.g. "Short offline", "Extended" */
	int64_t	 hours;		/* power-on hours when it ran */
	int8_t	 passed;	/* 1 passed, 0 failed, -1 unknown/aborted */
	uint8_t	 valid;		/* 0 if the log is empty */
};

struct disk {
	char		 path[32];	/* device node, e.g. /dev/ada0 */
	char		 model[48];
	char		 serial[32];	/* stable identity across reboots */
	char		 firmware[16];
	char		 error[80];	/* smartctl message when state is READ_ERROR */
	uint64_t	 capacity;	/* bytes */
	int64_t		 power_on_hours;	/* -1 if unknown */
	int32_t		 rotation_rate;	/* rpm, 0 solid state, -1 unknown */
	int16_t		 temp;		/* Celsius or TEMP_UNKNOWN */
	int8_t		 smart_passed;	/* 1 passed, 0 failing, -1 unknown */
	uint8_t		 exit_status;	/* smartctl exit bit mask */
	enum proto	 proto;
	enum read_state	 state;
	struct selftest	 last_test;
	union {
		struct {
			struct ata_attr attr[ATA_MAX_ATTRS];
			uint8_t nattr;
		} ata;
		struct nvme_health nvme;
	} u;
};

/*
 * List candidate devices with `smartctl --scan`. On success returns 0 and a
 * calloc'ed array of *n disks with only the path filled in; free() it.
 * b is a scratch buffer reused across calls.
 */
int	smart_scan(struct buf *b, struct disk **disks, size_t *n);

/*
 * Read SMART data for d->path, without waking a disk in standby.
 * Returns 0 when smartctl produced a report (d->state tells what it holds),
 * -1 with errno set when smartctl could not be run or its output is not JSON.
 */
int	smart_read(struct buf *b, struct disk *d);

/* Find an ATA attribute by id, NULL if the disk does not report it. */
const struct ata_attr *smart_ata_attr(const struct disk *d, uint8_t id);

#endif
