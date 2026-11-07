/* daemon.c - background collection and the refresh socket. */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "daemon.h"
#include "db.h"
#include "health.h"
#include "smart.h"

#define CLIENT_TIMEOUT_MS	2000		/* to send the request */
#define REFRESH_TIMEOUT_MS	(10 * 60 * 1000) /* for the daemon to answer */

/* What the daemon knows about each device node. */
struct slot {
	struct disk	d;
	time_t		next_full;	/* monotonic time of the next report */
	time_t		last_full;	/* monotonic time of the last one */
	time_t		asleep_since;	/* wall time of the first skip, or 0 */
	enum level	level;		/* last evaluation, for the log */
	uint8_t		known;		/* d holds a report of this disk */
};

struct daemon {
	struct config	 cfg;
	const char	*conf_path;
	struct db	 db;
	struct buf	 b;
	struct slot	*slots;
	size_t		 n;
	time_t		 next_scan;	/* monotonic */
	time_t		 next_temp;
	int		 listen_fd;
};

/* Signals are written to a pipe so that poll() wakes up without races. */
static int sig_pipe[2] = { -1, -1 };

static void
on_signal(int sig)
{
	int saved = errno;
	char c = (char)sig;
	ssize_t rc;

	rc = write(sig_pipe[1], &c, 1);	/* a full pipe already wakes us */
	(void)rc;
	errno = saved;
}

/* Seconds that never jump: schedules do not follow clock changes. */
static time_t
mono(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

static int
set_flags(int fd, int nonblock)
{
	int fl;

	if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
		return -1;
	if (!nonblock)
		return 0;
	if ((fl = fcntl(fd, F_GETFL)) == -1)
		return -1;
	return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int
socket_addr(const char *path, struct sockaddr_un *sa)
{
	memset(sa, 0, sizeof(*sa));
	sa->sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(sa->sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(sa->sun_path, path, strlen(path) + 1);
	return 0;
}

static int
connect_socket(const char *path)
{
	struct sockaddr_un sa;
	int fd;

	if (socket_addr(path, &sa) == -1 ||
	    (fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Listen on path, refusing to take it over from a running daemon. */
static int
listen_socket(const char *path, gid_t gid)
{
	struct sockaddr_un sa;
	int fd;

	if ((fd = connect_socket(path)) != -1) {
		close(fd);
		errno = EADDRINUSE;
		return -1;
	}
	if (socket_addr(path, &sa) == -1 ||
	    (fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return -1;
	unlink(path);		/* left over by a daemon that crashed */
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == -1 ||
	    chmod(path, 0660) == -1 ||
	    (gid != (gid_t)-1 && chown(path, (uid_t)-1, gid) == -1) ||
	    listen(fd, 4) == -1 || set_flags(fd, 1) == -1) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Wait up to ms for fd to be readable. */
static int
wait_readable(int fd, int ms)
{
	struct pollfd p = { fd, POLLIN, 0 };
	int rc;

	while ((rc = poll(&p, 1, ms)) == -1 && errno == EINTR)
		;
	if (rc == 0)
		errno = ETIMEDOUT;
	return rc > 0 ? 0 : -1;
}

static void
log_level_change(struct slot *s, const struct health *h)
{
	const char *name = s->d.path + (strncmp(s->d.path, "/dev/", 5) == 0 ?
	    5 : 0);
	int prio = h->level == LEVEL_CRIT ? LOG_CRIT :
	    h->level == LEVEL_WARN ? LOG_WARNING : LOG_NOTICE;

	if (s->known && h->level == s->level)
		return;
	if (h->n > 0)
		syslog(prio, "%s (%s): %s: %s%s", name, s->d.serial,
		    level_name(h->level), h->f[0].text,
		    h->n > 1 ? ", ..." : "");
	else
		syslog(prio, "%s (%s): %s", name, s->d.serial,
		    level_name(h->level));
	s->level = h->level;
}

/* Read one disk completely and store its report. */
static void
read_full(struct daemon *dm, struct slot *s, time_t now)
{
	struct disk t;
	struct health h;
	time_t wall = time(NULL);
	int wake;

	/* A disk that keeps sleeping is woken once its data gets too old. */
	wake = s->asleep_since != 0 &&
	    wall - s->asleep_since >= dm->cfg.wake_after;
	s->next_full = now + dm->cfg.standby_retry;
	memcpy(t.path, s->d.path, sizeof(t.path));
	if (smart_read(&dm->b, &t, wake ? READ_FULL_WAKE : READ_FULL) == -1) {
		syslog(LOG_ERR, "%s: smartctl: %s", t.path,
		    errno == EPROTO ? "output is not JSON" : strerror(errno));
		return;
	}
	if (db_store_node(&dm->db, &t, wall) == -1)
		syslog(LOG_ERR, "%s: database: %s", t.path, db_error(&dm->db));
	switch (t.state) {
	case READ_STANDBY:
		if (s->asleep_since == 0)
			s->asleep_since = wall;
		return;
	case READ_ERROR:
		syslog(LOG_WARNING, "%s: %s", t.path, t.error);
		return;
	case READ_OK:
		break;
	}
	s->d = t;	/* keep the last good report of the disk */
	s->asleep_since = 0;
	s->last_full = now;
	s->next_full = now + dm->cfg.smart_interval;
	if (s->d.proto == PROTO_OTHER)
		return;
	health_eval(&dm->cfg, &s->d, &h);
	log_level_change(s, &h);
	s->known = 1;
	if (db_store_report(&dm->db, &s->d, h.level, dm->b.data, dm->b.len,
	    wall) == -1)
		syslog(LOG_ERR, "%s: cannot store the report: %s", s->d.path,
		    db_error(&dm->db));
}

/* Rebuild the slot list from a scan, keeping what is known per node. */
static void
rescan(struct daemon *dm, time_t now)
{
	struct disk *found;
	struct slot *slots;
	size_t i, j, n;

	if (smart_scan(&dm->b, &found, &n) == -1) {
		syslog(LOG_ERR, "smartctl --scan: %s", strerror(errno));
		return;
	}
	if ((slots = calloc(n > 0 ? n : 1, sizeof(*slots))) == NULL) {
		syslog(LOG_ERR, "out of memory");
		free(found);
		return;
	}
	for (i = 0; i < n; i++) {
		for (j = 0; j < dm->n; j++)
			if (strcmp(dm->slots[j].d.path, found[i].path) == 0)
				break;
		if (j < dm->n)
			slots[i] = dm->slots[j];
		else
			memcpy(slots[i].d.path, found[i].path,
			    sizeof(slots[i].d.path));
		slots[i].next_full = now;	/* a scan reads every disk */
	}
	free(found);
	free(dm->slots);
	dm->slots = slots;
	dm->n = n;
}

static int
any_due(const struct daemon *dm, time_t now)
{
	size_t i;

	for (i = 0; i < dm->n; i++)
		if (now >= dm->slots[i].next_full)
			return 1;
	return 0;
}

/* Read the disks whose report is due; with `scan` find the disks first. */
static void
collect(struct daemon *dm, int scan)
{
	time_t now = mono(), wall = time(NULL);
	size_t i;

	if (scan) {
		rescan(dm, now);
		dm->next_scan = now + dm->cfg.smart_interval;
	}
	if (db_begin(&dm->db) == -1)
		goto fail;
	if (scan && db_seen_reset(&dm->db) == -1)
		goto fail;
	for (i = 0; i < dm->n; i++) {
		if (scan && db_seen_path(&dm->db, dm->slots[i].d.path) == -1)
			goto fail;
		if (now >= dm->slots[i].next_full)
			read_full(dm, &dm->slots[i], now);
	}
	if ((scan && (db_drop_unseen(&dm->db) == -1 ||
	    db_prune(&dm->db, wall - dm->cfg.temp_keep) == -1)) ||
	    db_heartbeat(&dm->db, wall) == -1 || db_commit(&dm->db) == -1)
		goto fail;
	return;
fail:
	syslog(LOG_ERR, "database: %s", db_error(&dm->db));
	db_rollback(&dm->db);
}

/* Sample the temperature of every disk not read in this interval. */
static void
sample_temps(struct daemon *dm)
{
	struct disk t;
	time_t now = mono(), wall = time(NULL);
	struct slot *s;
	size_t i;

	dm->next_temp = now + dm->cfg.temp_interval;
	if (db_begin(&dm->db) == -1)
		goto fail;
	for (i = 0; i < dm->n; i++) {
		s = &dm->slots[i];
		if (!s->known || now - s->last_full < dm->cfg.temp_interval)
			continue;
		memcpy(t.path, s->d.path, sizeof(t.path));
		if (smart_read(&dm->b, &t, READ_TEMP) == -1 ||
		    t.state != READ_OK)
			continue;
		/* Another disk behind the same node: read it fully. */
		if (strcmp(t.serial, s->d.serial) != 0) {
			s->next_full = now;
			continue;
		}
		if (db_store_temp(&dm->db, &t, wall) == -1)
			goto fail;
	}
	if (db_heartbeat(&dm->db, wall) == -1 || db_commit(&dm->db) == -1)
		goto fail;
	return;
fail:
	syslog(LOG_ERR, "database: %s", db_error(&dm->db));
	db_rollback(&dm->db);
}

/* Answer one request on the socket: "refresh" reads all disks now. */
static void
serve_client(struct daemon *dm)
{
	static const char ok[] = "ok\n", bad[] = "error: unknown request\n";
	char req[32];
	ssize_t n;
	int fd;

	if ((fd = accept(dm->listen_fd, NULL, NULL)) == -1)
		return;
	if (set_flags(fd, 0) == -1 ||
	    wait_readable(fd, CLIENT_TIMEOUT_MS) == -1 ||
	    (n = read(fd, req, sizeof(req) - 1)) <= 0) {
		close(fd);
		return;
	}
	req[n] = '\0';
	if (strcmp(req, "refresh\n") == 0) {
		collect(dm, 1);
		n = write(fd, ok, sizeof(ok) - 1);
	} else {
		n = write(fd, bad, sizeof(bad) - 1);
	}
	close(fd);	/* nothing to do if the client went away */
}

/* SIGHUP: reload the configuration; paths and the group need a restart. */
static void
reload(struct daemon *dm)
{
	struct config c;

	config_defaults(&c);
	if (dm->conf_path != NULL && config_load(&c, dm->conf_path, 1) == -1) {
		syslog(LOG_ERR, "%s: not reloaded, keeping the old rules",
		    dm->conf_path);
		return;
	}
	memcpy(c.database, dm->cfg.database, sizeof(c.database));
	memcpy(c.socket, dm->cfg.socket, sizeof(c.socket));
	memcpy(c.group, dm->cfg.group, sizeof(c.group));
	dm->cfg = c;
	syslog(LOG_NOTICE, "configuration reloaded");
}

/* Milliseconds until the next scheduled event. */
static int
next_timeout(const struct daemon *dm)
{
	time_t next = dm->next_scan < dm->next_temp ? dm->next_scan :
	    dm->next_temp, now = mono();
	size_t i;

	for (i = 0; i < dm->n; i++)
		if (dm->slots[i].next_full < next)
			next = dm->slots[i].next_full;
	if (next <= now)
		return 0;
	return next - now > INT_MAX / 1000 ? INT_MAX : (int)(next - now) * 1000;
}

static int
setup_signals(void)
{
	struct sigaction sa;

	if (pipe(sig_pipe) == -1 || set_flags(sig_pipe[0], 1) == -1 ||
	    set_flags(sig_pipe[1], 1) == -1)
		return -1;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGTERM, &sa, NULL) == -1 ||
	    sigaction(SIGINT, &sa, NULL) == -1 ||
	    sigaction(SIGHUP, &sa, NULL) == -1)
		return -1;
	sa.sa_handler = SIG_IGN;	/* a client that went away */
	return sigaction(SIGPIPE, &sa, NULL);
}

int
daemon_run(const struct config *cfg, const char *conf_path)
{
	struct daemon dm;
	struct pollfd pfd[2];
	struct group *gr;
	gid_t gid = (gid_t)-1;
	char sig;
	int stop = 0;

	memset(&dm, 0, sizeof(dm));
	dm.cfg = *cfg;
	dm.conf_path = conf_path;
	dm.listen_fd = -1;
	openlog("nascheck", LOG_PID | (isatty(STDERR_FILENO) ? LOG_PERROR : 0),
	    LOG_DAEMON);
	umask(027);

	if (cfg->group[0] != '\0') {
		if ((gr = getgrnam(cfg->group)) == NULL) {
			syslog(LOG_ERR, "group %s not found", cfg->group);
			return 1;
		}
		gid = gr->gr_gid;
	}
	if (setup_signals() == -1) {
		syslog(LOG_ERR, "signals: %s", strerror(errno));
		return 1;
	}
	if (db_open_writer(&dm.db, cfg->database, gid) == -1) {
		syslog(LOG_ERR, "%s: %s", cfg->database, db_error(&dm.db));
		db_close(&dm.db);
		return 1;
	}
	if ((dm.listen_fd = listen_socket(cfg->socket, gid)) == -1) {
		syslog(LOG_ERR, "%s: %s", cfg->socket, errno == EADDRINUSE ?
		    "another nascheck daemon is running" : strerror(errno));
		db_close(&dm.db);
		return 1;
	}
	syslog(LOG_NOTICE, "started, database %s", cfg->database);

	dm.next_scan = dm.next_temp = mono();
	pfd[0].fd = sig_pipe[0];
	pfd[0].events = POLLIN;
	pfd[1].fd = dm.listen_fd;
	pfd[1].events = POLLIN;
	while (!stop) {
		if (poll(pfd, 2, next_timeout(&dm)) == -1 && errno != EINTR) {
			syslog(LOG_ERR, "poll: %s", strerror(errno));
			break;
		}
		while (read(sig_pipe[0], &sig, 1) == 1) {
			if (sig == SIGHUP)
				reload(&dm);
			else
				stop = 1;
		}
		if (stop)
			break;
		if (pfd[1].revents & POLLIN)
			serve_client(&dm);
		/* A scan reads every disk; between scans only those due. */
		if (mono() >= dm.next_scan)
			collect(&dm, 1);
		else if (any_due(&dm, mono()))
			collect(&dm, 0);
		if (mono() >= dm.next_temp)
			sample_temps(&dm);
	}

	syslog(LOG_NOTICE, "stopped");
	close(dm.listen_fd);
	unlink(cfg->socket);
	db_close(&dm.db);
	buf_free(&dm.b);
	free(dm.slots);
	return 0;
}

int
daemon_refresh(const struct config *cfg)
{
	static const char req[] = "refresh\n";
	char reply[64];
	ssize_t n = -1;
	int fd;

	if ((fd = connect_socket(cfg->socket)) == -1) {
		fprintf(stderr, "nascheck: cannot reach the daemon at %s: %s\n",
		    cfg->socket, strerror(errno));
		return -1;
	}
	if (write(fd, req, sizeof(req) - 1) != (ssize_t)(sizeof(req) - 1) ||
	    wait_readable(fd, REFRESH_TIMEOUT_MS) == -1 ||
	    (n = read(fd, reply, sizeof(reply) - 1)) <= 0) {
		fprintf(stderr, "nascheck: no answer from the daemon: %s\n",
		    n == 0 ? "connection closed" : strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	reply[n] = '\0';
	if (strcmp(reply, "ok\n") != 0) {
		fprintf(stderr, "nascheck: daemon: %s", reply);
		return -1;
	}
	return 0;
}
