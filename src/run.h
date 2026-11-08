/* run.h - run an external command and capture its standard output. */
#ifndef NASCHECK_RUN_H
#define NASCHECK_RUN_H

#include <stddef.h>

/* Growable byte buffer, meant to be reused across calls to limit allocations. */
struct buf {
	char	*data;
	size_t	 len;	/* bytes of output, excluding the zero padding */
	size_t	 cap;	/* allocated bytes */
};

/*
 * Run argv[0] (searched in PATH) with stdin and stderr on /dev/null and
 * capture stdout in b. The output is followed by at least `pad` zero bytes,
 * so it is always a C string and can be parsed in place.
 * On success returns 0 and stores the exit code in *code (-1 if the child
 * was killed by a signal). Returns -1 with errno set if the command cannot
 * be run or its output exceeds RUN_MAX_OUTPUT.
 */
int	run_capture(char *const argv[], struct buf *b, size_t pad, int *code);

/* Copy len bytes into b, followed by a zero byte. Returns 0 or -1. */
int	buf_set(struct buf *b, const char *data, size_t len);

/* Append len bytes to b, followed by a zero byte. Returns 0 or -1. */
int	buf_append(struct buf *b, const char *data, size_t len);

/* Append the file at path to b, followed by a zero byte. Returns 0 or -1. */
int	buf_append_file(struct buf *b, const char *path);

void	buf_free(struct buf *b);

#define RUN_MAX_OUTPUT	(8u << 20)	/* 8 MiB, far above any smartctl report */

#endif
