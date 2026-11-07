/* daemon.h - background collection and the refresh socket. */
#ifndef NASCHECK_DAEMON_H
#define NASCHECK_DAEMON_H

#include "config.h"

/*
 * Run the collector in the foreground until SIGTERM or SIGINT; SIGHUP
 * reloads the rules from conf_path. rc.d and systemd take care of running
 * it in the background. Returns the process exit code.
 */
int	daemon_run(const struct config *cfg, const char *conf_path);

/*
 * Ask the running daemon to read all disks now and wait until it has
 * stored the results. Returns 0, or -1 after reporting the problem.
 */
int	daemon_refresh(const struct config *cfg);

#endif
