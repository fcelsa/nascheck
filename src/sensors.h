/* sensors.h - the temperature sensors of the machine. */
#ifndef NASCHECK_SENSORS_H
#define NASCHECK_SENSORS_H

#define SENSORS_MAX	64

/* Temperatures in tenths of a degree Celsius; 0 for a limit not known. */
struct sensor {
	char	chip[24];	/* "k10temp", "coretemp", "nvme", "cpu", "acpi" */
	int	dev;		/* which chip: two nvme are two devices */
	char	label[24];	/* "Tctl", "Core 0", "Composite", "tz0" */
	int	temp;
	int	high;		/* the chip's own limits */
	int	crit;
};

struct sensors {
	struct sensor	s[SENSORS_MAX];
	int		n;
};

/*
 * Linux: /sys/class/hwmon, or the thermal zones when there is none.
 * FreeBSD: dev.cpu.N.temperature (coretemp, amdtemp) and the ACPI zones.
 * Sensors of a chip are next to each other.
 */
void	sensors_read(struct sensors *ss);

#endif
