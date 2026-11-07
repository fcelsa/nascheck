/* config.c - nascheck configuration file. */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#define LINE_MAX_LEN	512

#ifdef __linux__
#define DEFAULT_DATABASE	"/var/lib/nascheck/nascheck.db"
#else
#define DEFAULT_DATABASE	"/var/db/nascheck/nascheck.db"
#endif
#define DEFAULT_SOCKET		"/var/run/nascheck.sock"

#define MINUTE	60
#define HOUR	(60 * MINUTE)
#define DAY	(24 * HOUR)
#define MAX_TOKENS	4	/* keyword and up to three values */

/* Keep in sync with nascheck.conf.sample. */
static const struct config defaults = {
	.database = DEFAULT_DATABASE,
	.socket = DEFAULT_SOCKET,
	.group = "",
	.smart_interval = DAY,
	.temp_interval = 5 * MINUTE,
	.standby_retry = HOUR,
	.wake_after = 7 * DAY,
	.temp_keep = 90 * DAY,
	.temp_hdd = { 45, 55 },
	.temp_ssd = { 60, 70 },
	.temp_nvme = { LIMIT_AUTO, LIMIT_AUTO },
	.nvme_used = { 80, 100 },
	.nvme_media_errors = { 1, LIMIT_OFF },
	.selftest_max_age = 720,
	.ata = {
		{ { 1, LIMIT_OFF }, ATTR_REALLOC },
		{ { 1, LIMIT_OFF }, ATTR_REPORTED_UNCORRECT },
		{ { 1, LIMIT_OFF }, ATTR_PENDING },
		{ { 1, LIMIT_OFF }, ATTR_OFFLINE },
	},
	.nata = 4,
};

void
config_defaults(struct config *c)
{
	*c = defaults;
}

/* A threshold: a number, "-" for none or, where allowed, "auto". */
static const char *
parse_value(const char *s, int allow_auto, int64_t *v)
{
	char *end;
	long long n;

	if (strcmp(s, "-") == 0) {
		*v = LIMIT_OFF;
		return NULL;
	}
	if (strcmp(s, "auto") == 0) {
		if (!allow_auto)
			return "\"auto\" is only valid for temperatures";
		*v = LIMIT_AUTO;
		return NULL;
	}
	errno = 0;
	n = strtoll(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || n < 0 || n > INT32_MAX)
		return "expected a number, \"-\" or \"auto\"";
	*v = n;
	return NULL;
}

/* A duration: a number with an s, m, h or d suffix; seconds without. */
static const char *
parse_duration(const char *s, int64_t *v)
{
	char *end;
	long long n;

	errno = 0;
	n = strtoll(s, &end, 10);
	if (errno != 0 || end == s || n <= 0 || n > INT32_MAX)
		return "expected a positive duration such as 30s, 5m, 1h, 7d";
	switch (*end) {
	case '\0':
	case 's':
		break;
	case 'm':
		n *= MINUTE;
		break;
	case 'h':
		n *= HOUR;
		break;
	case 'd':
		n *= DAY;
		break;
	default:
		return "expected a positive duration such as 30s, 5m, 1h, 7d";
	}
	if (*end != '\0' && end[1] != '\0')
		return "expected a positive duration such as 30s, 5m, 1h, 7d";
	*v = n;
	return NULL;
}

/* A string value that must fit in its field. */
static const char *
parse_string(char **tok, int n, char *dst, size_t size)
{
	if (n != 1)
		return "expected one value";
	if (strlen(tok[0]) >= size)
		return "value too long";
	memcpy(dst, tok[0], strlen(tok[0]) + 1);
	return NULL;
}

/* "warn [crit]": a single value leaves the critical level off,
 * except "auto" which applies to both. */
static const char *
parse_limit(char **tok, int n, int allow_auto, struct limit *lim)
{
	const char *err;

	if (n < 1 || n > 2)
		return "expected a warning and an optional critical limit";
	if ((err = parse_value(tok[0], allow_auto, &lim->warn)) != NULL)
		return err;
	if (n == 2)
		return parse_value(tok[1], allow_auto, &lim->crit);
	lim->crit = lim->warn == LIMIT_AUTO ? LIMIT_AUTO : LIMIT_OFF;
	return NULL;
}

/* "ata_attr id warn [crit]". The first one in a file replaces the
 * default list, so that attributes can also be removed. */
static const char *
parse_ata(struct config *c, char **tok, int n, int *replaced)
{
	struct limit lim;
	const char *err;
	int64_t id;
	uint8_t i;

	if (n < 2)
		return "expected an attribute id and its limits";
	if (parse_value(tok[0], 0, &id) != NULL || id < 1 || id > 255)
		return "attribute id must be between 1 and 255";
	if ((err = parse_limit(tok + 1, n - 1, 0, &lim)) != NULL)
		return err;
	if (!*replaced) {
		c->nata = 0;
		*replaced = 1;
	}
	for (i = 0; i < c->nata && c->ata[i].id != id; i++)
		;
	if (i == ATA_MAX_ATTRS)
		return "too many attributes";
	if (i == c->nata)
		c->nata++;
	c->ata[i].id = (uint8_t)id;
	c->ata[i].lim = lim;
	return NULL;
}

/*
 * Parse one line in place. Returns an error message or NULL; *key points
 * to the keyword, for the message.
 */
static const char *
parse_line(struct config *c, char *line, int *ata_replaced, const char **key)
{
	static const struct {
		const char	*key;
		size_t		 off;
	} durations[] = {
		{ "smart_interval", offsetof(struct config, smart_interval) },
		{ "temp_interval", offsetof(struct config, temp_interval) },
		{ "standby_retry", offsetof(struct config, standby_retry) },
		{ "wake_after", offsetof(struct config, wake_after) },
		{ "temp_keep", offsetof(struct config, temp_keep) },
	};
	char *tok[MAX_TOKENS], *p, *save;
	size_t i;
	int n = 0;

	if ((p = strchr(line, '#')) != NULL)
		*p = '\0';
	for (p = strtok_r(line, " \t\r\n", &save); p != NULL;
	    p = strtok_r(NULL, " \t\r\n", &save)) {
		if (n == MAX_TOKENS)
			return "too many values";
		tok[n++] = p;
	}
	if (n == 0)
		return NULL;
	*key = tok[0];

	if (strcmp(tok[0], "database") == 0)
		return parse_string(tok + 1, n - 1, c->database,
		    sizeof(c->database));
	if (strcmp(tok[0], "socket") == 0)
		return parse_string(tok + 1, n - 1, c->socket,
		    sizeof(c->socket));
	if (strcmp(tok[0], "group") == 0)
		return parse_string(tok + 1, n - 1, c->group, sizeof(c->group));
	for (i = 0; i < sizeof(durations) / sizeof(durations[0]); i++) {
		if (strcmp(tok[0], durations[i].key) != 0)
			continue;
		if (n != 2)
			return "expected one value";
		return parse_duration(tok[1],
		    (int64_t *)(void *)((char *)c + durations[i].off));
	}
	if (strcmp(tok[0], "temp_hdd") == 0)
		return parse_limit(tok + 1, n - 1, 1, &c->temp_hdd);
	if (strcmp(tok[0], "temp_ssd") == 0)
		return parse_limit(tok + 1, n - 1, 1, &c->temp_ssd);
	if (strcmp(tok[0], "temp_nvme") == 0)
		return parse_limit(tok + 1, n - 1, 1, &c->temp_nvme);
	if (strcmp(tok[0], "nvme_used") == 0)
		return parse_limit(tok + 1, n - 1, 0, &c->nvme_used);
	if (strcmp(tok[0], "nvme_media_errors") == 0)
		return parse_limit(tok + 1, n - 1, 0, &c->nvme_media_errors);
	if (strcmp(tok[0], "selftest_max_age") == 0) {
		if (n != 2)
			return "expected one value";
		return parse_value(tok[1], 0, &c->selftest_max_age);
	}
	if (strcmp(tok[0], "ata_attr") == 0)
		return parse_ata(c, tok + 1, n - 1, ata_replaced);
	return "unknown keyword";
}

int
config_load(struct config *c, const char *path, int optional)
{
	char line[LINE_MAX_LEN];
	const char *err = NULL, *key = NULL;
	int lineno = 0, ata_replaced = 0;
	FILE *f;

	if ((f = fopen(path, "r")) == NULL) {
		if (optional && errno == ENOENT)
			return 0;
		fprintf(stderr, "nascheck: %s: %s\n", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		if (strchr(line, '\n') == NULL && !feof(f)) {
			err = "line too long";
			break;
		}
		key = NULL;
		if ((err = parse_line(c, line, &ata_replaced, &key)) != NULL)
			break;
	}
	if (err == NULL && ferror(f))
		err = strerror(errno);
	fclose(f);
	if (err != NULL) {
		fprintf(stderr, "nascheck: %s:%d: %s%s%s\n", path, lineno,
		    key != NULL ? key : "", key != NULL ? ": " : "", err);
		return -1;
	}
	return 0;
}
