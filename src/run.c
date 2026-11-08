/* run.c - run an external command and capture its standard output. */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "run.h"

extern char **environ;

#define BUF_INITIAL	(64u << 10)	/* fits a typical `smartctl -a -j` report */

/* Make room for at least `need` bytes; grows geometrically. */
static int
buf_reserve(struct buf *b, size_t need)
{
	size_t cap;
	char *p;

	if (need <= b->cap)
		return 0;
	cap = b->cap ? b->cap : BUF_INITIAL;
	while (cap < need)
		cap *= 2;
	if ((p = realloc(b->data, cap)) == NULL)
		return -1;
	b->data = p;
	b->cap = cap;
	return 0;
}

int
buf_set(struct buf *b, const char *data, size_t len)
{
	if (buf_reserve(b, len + 1) == -1)
		return -1;
	memcpy(b->data, data, len);
	b->data[len] = '\0';
	b->len = len;
	return 0;
}

int
buf_append(struct buf *b, const char *data, size_t len)
{
	if (buf_reserve(b, b->len + len + 1) == -1)
		return -1;
	memcpy(b->data + b->len, data, len);
	b->len += len;
	b->data[b->len] = '\0';
	return 0;
}

int
buf_append_file(struct buf *b, const char *path)
{
	char chunk[4096];
	size_t n;
	FILE *f;
	int rc = 0;

	if ((f = fopen(path, "r")) == NULL)
		return -1;
	while (rc == 0 && (n = fread(chunk, 1, sizeof(chunk), f)) > 0)
		rc = buf_append(b, chunk, n);
	fclose(f);
	return rc;
}

void
buf_free(struct buf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}

/* Read everything from fd into b, leaving `pad` zero bytes after the data. */
static int
read_all(int fd, struct buf *b, size_t pad)
{
	ssize_t n;

	b->len = 0;
	for (;;) {
		/* Always keep room for the padding plus a reasonable read. */
		if (buf_reserve(b, b->len + pad + 4096) == -1)
			return -1;
		n = read(fd, b->data + b->len, b->cap - b->len - pad);
		if (n == 0)
			break;
		if (n == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		b->len += (size_t)n;
		if (b->len > RUN_MAX_OUTPUT) {
			errno = EFBIG;
			return -1;
		}
	}
	memset(b->data + b->len, 0, pad);
	return 0;
}

int
run_capture(char *const argv[], struct buf *b, size_t pad, int *code)
{
	posix_spawn_file_actions_t fa;
	pid_t pid;
	int fds[2], status, err, rc = -1;

	if (pad == 0)
		pad = 1;	/* room for the string terminator */
	if (pipe(fds) == -1)
		return -1;

	/* Child: stdout -> pipe, stdin/stderr -> /dev/null. */
	if ((err = posix_spawn_file_actions_init(&fa)) != 0)
		goto close_pipe;
	if ((err = posix_spawn_file_actions_addopen(&fa, 0, "/dev/null",
	    O_RDONLY, 0)) != 0 ||
	    (err = posix_spawn_file_actions_adddup2(&fa, fds[1], 1)) != 0 ||
	    (err = posix_spawn_file_actions_addopen(&fa, 2, "/dev/null",
	    O_WRONLY, 0)) != 0 ||
	    (err = posix_spawn_file_actions_addclose(&fa, fds[0])) != 0 ||
	    (err = posix_spawn_file_actions_addclose(&fa, fds[1])) != 0)
		goto destroy_fa;
	err = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);

destroy_fa:
	posix_spawn_file_actions_destroy(&fa);
close_pipe:
	close(fds[1]);
	if (err != 0) {
		close(fds[0]);
		errno = err;
		return -1;
	}

	/* Read first, then reap: the child may block on a full pipe. */
	if (read_all(fds[0], b, pad) == 0)
		rc = 0;
	err = errno;
	close(fds[0]);	/* if we stopped early the child gets SIGPIPE */

	while (waitpid(pid, &status, 0) == -1) {
		if (errno != EINTR)
			return -1;
	}
	*code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	errno = err;
	return rc;
}
