/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C) 2022-2023 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"

#if defined(NSIG)
#define MAX_SIGNUM      NSIG
#elif defined(_NSIG)
#define MAX_SIGNUM      _NSIG
#else
#define MAX_SIGNUM      (256)
#endif

#define MAX_BACKOFF	(10000)

static const stress_help_t help[] = {
	{ NULL,	"daemon N",	"start N workers creating multiple daemons" },
	{ NULL,	"daemon-ops N",	"stop when N daemons have been created" },
	{ NULL, "daemon-wait",	"stressor wait for daemon to exit and not init" },
	{ NULL,	NULL,		NULL }
};

static int stress_daemon_set_daemon_wait(const char *opt)
{
	bool daemon_wait = true;
	(void)opt;

	return stress_set_setting("daemon-wait", TYPE_ID_BOOL, &daemon_wait);
}

/*
 *  stress_daemon_set_daemon_wait()
 *	waits for child if daemon_wait is set, otherwise let init do it
 */
static void daemon_wait_pid(const pid_t pid, const bool daemon_wait)
{
	if (daemon_wait) {
		int status;

		VOID_RET(int, waitpid(pid, &status, 0));
	}
}

/*
 *  daemons()
 *	fork off a child and let the parent die
 */
static void daemons(
	const stress_args_t *args,
	const int fd,
	const bool daemon_wait)
{
	int fds[3];
	int i;
	sigset_t set;
	uint64_t backoff = 100;

	if (stress_sig_stop_stressing(args->name, SIGALRM) < 0)
		goto err;
	if (setsid() < 0)
		goto err;

	(void)close(0);
	(void)close(1);
	(void)close(2);

	for (i = 0; i < MAX_SIGNUM; i++)
		(void)signal(i, SIG_DFL);

	(void)sigemptyset(&set);
	(void)sigprocmask(SIG_SETMASK, &set, NULL);
#if defined(HAVE_CLEARENV)
	(void)clearenv();
#endif

	if ((fds[0] = open("/dev/null", O_RDWR)) < 0)
		goto err;
	if ((fds[1] = dup(0)) < 0)
		goto err0;
	if ((fds[2] = dup(0)) < 0)
		goto err1;

	while (keep_stressing_flag()) {
		pid_t pid;

		pid = fork();
		if (pid < 0) {
			/* A slow init? no pids or memory, retry */
			if ((errno == EAGAIN) || (errno == ENOMEM)) {
				/* Minor backoff before retrying */
				(void)shim_usleep_interruptible(backoff);
				backoff += 100;
				if (backoff > MAX_BACKOFF)
					backoff = MAX_BACKOFF;
				continue;
			}
			goto tidy;
		} else if (pid == 0) {
			/* Child */
			uint8_t buf[1] = { 0xff };
			ssize_t sz;

			if (chdir("/") < 0)
				goto err2;
			(void)umask(0);
			VOID_RET(int, stress_drop_capabilities(args->name));

			sz = write(fd, buf, sizeof(buf));
			if (sz != sizeof(buf))
				goto err2;
		} else {
			/* Parent, will be reaped by init unless daemon_wait is true */
			daemon_wait_pid(pid, daemon_wait);
			break;
		}
	}

tidy:
	(void)close(fds[2]);
	(void)close(fds[1]);
	(void)close(fds[0]);
	return;

err2:	(void)close(fds[2]);
err1:	(void)close(fds[1]);
err0: 	(void)close(fds[0]);
err:	(void)close(fd);
}

/*
 *  stress_daemon()
 *	stress by multiple daemonizing forks
 */
static int stress_daemon(const stress_args_t *args)
{
	int fds[2];
	pid_t pid;
	bool daemon_wait = false;

	(void)stress_get_setting("daemon-wait", &daemon_wait);

	if (stress_sig_stop_stressing(args->name, SIGALRM) < 0)
		return EXIT_FAILURE;

	if (pipe(fds) < 0) {
		pr_fail("%s: pipe failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		return EXIT_FAILURE;
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);
again:
	pid = fork();
	if (pid < 0) {
		if (stress_redo_fork(errno))
			goto again;
		if (!keep_stressing(args))
			goto finish;
		pr_fail("%s: fork failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		(void)close(fds[0]);
		(void)close(fds[1]);
		return EXIT_FAILURE;
	} else if (pid == 0) {
		/* Children */
		(void)close(fds[0]);
		daemons(args, fds[1], daemon_wait);
		(void)close(fds[1]);
		shim_exit_group(0);
	} else {
		/* Parent */
		(void)close(fds[1]);
		do {
			ssize_t n;
			char buf[1];

			n = read(fds[0], buf, sizeof(buf));
			if (n < 0) {
				(void)close(fds[0]);
				if (errno != EINTR) {
					pr_dbg("%s: read failed: "
						"errno=%d (%s)\n",
						args->name,
						errno, strerror(errno));
				}
				break;
			}
			inc_counter(args);
		} while (keep_stressing(args));

		daemon_wait_pid(pid, daemon_wait);
	}

finish:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	return EXIT_SUCCESS;
}

static const stress_opt_set_func_t opt_set_funcs[] = {
        { OPT_daemon_wait,	stress_daemon_set_daemon_wait },
	{ 0,			NULL },
};

stressor_info_t stress_daemon_info = {
	.stressor = stress_daemon,
	.class = CLASS_SCHEDULER | CLASS_OS,
	.opt_set_funcs = opt_set_funcs,
	.verify = VERIFY_ALWAYS,
	.help = help
};
