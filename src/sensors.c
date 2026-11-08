/* sensors.c - the temperature sensors of the machine. */
#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sensors.h"

static struct sensor *
add(struct sensors *ss, int dev, const char *chip, const char *label,
    int temp)
{
	struct sensor *s;

	if (ss->n >= SENSORS_MAX)
		return NULL;
	s = &ss->s[ss->n++];
	s->dev = dev;
	snprintf(s->chip, sizeof(s->chip), "%s", chip);
	snprintf(s->label, sizeof(s->label), "%s", label);
	s->temp = temp;
	s->high = s->crit = 0;
	return s;
}

#ifdef __FreeBSD__

/* A temperature sysctl (tenths of a kelvin) in tenths of a degree. */
static int
deci_celsius(const char *name, int *out)
{
	size_t len = sizeof(int);
	int v;

	if (sysctlbyname(name, &v, &len, NULL, 0) == -1 || len != sizeof(v) ||
	    v <= 0)
		return -1;	/* missing, or -1: not provided */
	*out = v - 2731;
	return 0;
}

void
sensors_read(struct sensors *ss)
{
	char name[64], label[24];
	struct sensor *s;
	int i, v, crit;

	ss->n = 0;
	/* coretemp(4) and amdtemp(4): one per CPU. */
	for (i = 0;; i++) {
		snprintf(name, sizeof(name), "dev.cpu.%d.temperature", i);
		if (deci_celsius(name, &v) == -1)
			break;
		snprintf(label, sizeof(label), "CPU %d", i);
		if ((s = add(ss, 0, "cpu", label, v)) == NULL)
			return;
		snprintf(name, sizeof(name), "dev.cpu.%d.coretemp.tjmax", i);
		if (deci_celsius(name, &crit) == 0)
			s->crit = crit;
	}
	/* ACPI thermal zones, with their critical point. */
	for (i = 0;; i++) {
		snprintf(name, sizeof(name), "hw.acpi.thermal.tz%d.temperature",
		    i);
		if (deci_celsius(name, &v) == -1)
			break;
		snprintf(label, sizeof(label), "tz%d", i);
		if ((s = add(ss, 1, "acpi", label, v)) == NULL)
			return;
		snprintf(name, sizeof(name), "hw.acpi.thermal.tz%d._CRT", i);
		if (deci_celsius(name, &crit) == 0)
			s->crit = crit;
	}
}

#else	/* Linux */

/* Tests: NASCHECK_ROOT stands for /. */
static const char *
root(void)
{
	const char *r = getenv("NASCHECK_ROOT");

	return r != NULL ? r : "";
}

/* A number from a sysfs file. */
static int
read_long(const char *path, long *v)
{
	FILE *f;
	int ok;

	if ((f = fopen(path, "r")) == NULL)
		return -1;
	ok = fscanf(f, "%ld", v) == 1;
	fclose(f);
	return ok ? 0 : -1;
}

/* The first line of a sysfs file, "" if it cannot be read. */
static void
read_line(const char *path, char *buf, size_t size)
{
	FILE *f;

	buf[0] = '\0';
	if ((f = fopen(path, "r")) == NULL)
		return;
	if (fgets(buf, (int)size, f) == NULL)
		buf[0] = '\0';
	buf[strcspn(buf, "\n")] = '\0';
	fclose(f);
}

/* hwmon2 before hwmon10. */
static int
by_number(const struct dirent **a, const struct dirent **b)
{
	size_t la = strlen((*a)->d_name), lb = strlen((*b)->d_name);

	return la != lb ? (la < lb ? -1 : 1) :
	    strcmp((*a)->d_name, (*b)->d_name);
}

/* One hwmon chip: temp1_input, temp2_input... with label, max, crit. */
static void
hwmon_chip(struct sensors *ss, int dev, const char *dir)
{
	char path[640], chip[24], label[24];
	struct sensor *s;
	long v;
	int i;

	snprintf(path, sizeof(path), "%s/name", dir);
	read_line(path, chip, sizeof(chip));
	/* Inputs are numbered from 1, maybe with gaps (coretemp cores). */
	for (i = 1; i <= 128; i++) {
		snprintf(path, sizeof(path), "%s/temp%d_input", dir, i);
		if (read_long(path, &v) == -1 || v <= -40000 || v >= 150000)
			continue;	/* missing, or not a real reading */
		snprintf(path, sizeof(path), "%s/temp%d_label", dir, i);
		read_line(path, label, sizeof(label));
		if (label[0] == '\0')
			snprintf(label, sizeof(label), "temp%d", i);
		if ((s = add(ss, dev, chip[0] != '\0' ? chip : "?", label,
		    (int)(v / 100))) == NULL)
			return;
		snprintf(path, sizeof(path), "%s/temp%d_max", dir, i);
		if (read_long(path, &v) == 0 && v > 0)
			s->high = (int)(v / 100);
		snprintf(path, sizeof(path), "%s/temp%d_crit", dir, i);
		if (read_long(path, &v) == 0 && v > 0)
			s->crit = (int)(v / 100);
	}
}

void
sensors_read(struct sensors *ss)
{
	struct dirent **d;
	char dir[256], sub[600], path[640], type[24];
	long v;
	int i, n;

	ss->n = 0;
	snprintf(dir, sizeof(dir), "%s/sys/class/hwmon", root());
	if ((n = scandir(dir, &d, NULL, by_number)) >= 0) {
		for (i = 0; i < n; i++) {
			if (d[i]->d_name[0] != '.') {
				snprintf(sub, sizeof(sub), "%s/%s", dir,
				    d[i]->d_name);
				/* Old kernels keep the files one level down. */
				snprintf(path, sizeof(path), "%s/name", sub);
				if (access(path, R_OK) != 0)
					strcat(sub, "/device");
				hwmon_chip(ss, i, sub);
			}
			free(d[i]);
		}
		free(d);
	}
	if (ss->n > 0)
		return;
	/* No hwmon: the thermal zones (the same sensors, fewer details). */
	for (i = 0; i < SENSORS_MAX; i++) {
		snprintf(path, sizeof(path),
		    "%s/sys/class/thermal/thermal_zone%d/temp", root(), i);
		if (read_long(path, &v) == -1)
			break;
		snprintf(path, sizeof(path),
		    "%s/sys/class/thermal/thermal_zone%d/type", root(), i);
		read_line(path, type, sizeof(type));
		if (add(ss, i, "thermal", type[0] != '\0' ? type : "zone",
		    (int)(v / 100)) == NULL)
			return;
	}
}

#endif
