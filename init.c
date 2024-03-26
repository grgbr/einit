#include "repo.h"
#include "target.h"
#include "log.h"
#include "mnt.h"
#include "sigchan.h"
#include "srv.h"
#include "proto.h"
#include <stroll/cdefs.h>
#include <utils/path.h>
#include <utils/signal.h>
#include <utils/string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>

static const char * tinit_boot_target = "current";

static void
tinit_parse_target_arg(char * arg, size_t len)
{
	assert(arg);
	assert(len);
	assert((size_t)ustr_parse(arg, TINIT_ARG_MAX) == len);

	if (tinit_check_svc_name(arg, len)) {
		tinit_warn("invalid target argument.");
		return;
	}

	tinit_boot_target = arg;
}

typedef void (tinit_cmdln_parser_fn)(char * arg, size_t len);

struct tinit_cmdln_parser {
	const char *            kword;
	size_t                  len;
	tinit_cmdln_parser_fn * parse;
};

#define TINIT_INIT_CMD_PARSER(_kword, _parse) \
	{ \
		.kword = _kword, \
		.len   = sizeof(_kword) - 1, \
		.parse = _parse \
	}

static const struct tinit_cmdln_parser tinit_cmdln_parsers[] = {
	TINIT_INIT_CMD_PARSER("stdlog", tinit_parse_stdlog_arg),
	TINIT_INIT_CMD_PARSER("mqlog",  tinit_parse_mqlog_arg),
	TINIT_INIT_CMD_PARSER("target", tinit_parse_target_arg)
};

static void
tinit_parse_arg(char * arg)
{
	char *       val;
	size_t       len;
	size_t       alen;
	unsigned int p;

	len = strnlen(arg, TINIT_ARG_MAX);
	assert(len);
	if (len >= TINIT_ARG_MAX) {
		tinit_warn("invalid argument.");
		return;
	}

	val = memchr(arg, '=', len);
	if (!val)
		goto warn;

	alen = (size_t)(val - arg);
	if (!alen || (alen >= len))
		goto warn;

	val++;
	for (p = 0; p < stroll_array_nr(tinit_cmdln_parsers); p++) {
		const struct tinit_cmdln_parser * parser;

		parser = &tinit_cmdln_parsers[p];

		if (ustr_match_token(arg, alen, parser->kword, parser->len)) {
			parser->parse(val, len - (val - arg));
			return;
		}
	}

warn:
	tinit_warn("invalid '%s' argument.", arg);
}

static void
tinit_parse_cmdln(int argc, char * const argv[])
{
	int a;

	for (a = 1; a < argc; a++)
		tinit_parse_arg(argv[a]);
}

static void
init_signals(void)
{
	sigset_t         msk;
	struct sigaction act = {
		.sa_handler = SIG_DFL,
		.sa_flags   = SA_NOCLDSTOP
	};

	usig_emptyset(&sig_empty_msk);

	/*
	 * No need to clear the Glibc's internal ]SIGSYS:SIGRTMIN[ range as it
	 * has already been done by Glibc.
	 */
	usig_fillset(&sig_full_msk);
	usig_delset(&sig_full_msk, SIGKILL);
	usig_delset(&sig_full_msk, SIGSTOP);

	/* Build and apply the full set of blocked signals. */
	msk = sig_full_msk;
	usig_delset(&msk, SIGILL);
	usig_delset(&msk, SIGABRT);
	usig_delset(&msk, SIGFPE);
	usig_delset(&msk, SIGSEGV);
	usig_delset(&msk, SIGBUS);
	usig_procmask(SIG_BLOCK, &msk, NULL);

	act.sa_mask = sig_empty_msk;
	usig_action(SIGCHLD, &act, NULL);
}

static int
init_environ(void)
{
	int err;

	if (clearenv()) {
		err = errno;

		tinit_err("cannot clear environment: %s (%d).",
		          strerror(err),
		          err);
		return -err;
	}

	err = putenv("HOME=/");
	assert(!err);
	err = putenv("PATH=" CONFIG_TINIT_ENVIRON_PATH);
	assert(!err);
	err = putenv("TERM=" CONFIG_TINIT_ENVIRON_TERM);
	assert(!err);

	tinit_debug("environment initialized.");

	return 0;
}

static int
init_stdios(void)
{
	int         null_fd;
	int         cons_fd;
	int         fd;
	struct stat stat;
	int         err;

	null_fd = sys_open_stdio("/dev/null",
	                         O_RDONLY | O_NOATIME | O_NOCTTY | O_NOFOLLOW |
	                         O_NONBLOCK);
	if (null_fd < 0)
		return null_fd;

	err = sys_fstat(null_fd, &stat);
	if (err)
		return err;

	if (!S_ISCHR(stat.st_mode) ||
	    ((stat.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) !=
	     (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) ||
	    stat.st_uid || stat.st_gid) {
		tinit_err("unexpected null terminal type or permissions.");
		return -EPERM;
	}

	cons_fd = sys_open_stdio("/dev/console",
	                    O_WRONLY | O_APPEND | O_NOATIME | O_NOCTTY |
	                    O_NOFOLLOW | O_NONBLOCK);
	if (cons_fd < 0)
		return cons_fd;

	err = sys_fstat(cons_fd, &stat);
	if (err)
		return err;

#if !defined(DOCKER)
	if (!S_ISCHR(stat.st_mode) ||
	    ((stat.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) !=
	     (S_IRUSR | S_IWUSR)) ||
	    stat.st_uid || stat.st_gid) {
		tinit_err("unexpected console terminal type or permissions.");
		return -EPERM;
	}
#endif

	err = sys_dup2(null_fd, STDIN_FILENO);
	if (err)
		return err;
	err = sys_dup2(cons_fd, STDOUT_FILENO);
	if (err)
		return err;
	err = sys_dup2(cons_fd, STDERR_FILENO);
	if (err)
		return err;

	/*
	 * Release controlling terminal if any.
	 *
	 * As we are init as the first session leader and running into the
	 * initial foreground process group, we may have been assigned a
	 * controlling tty (especially while running from a container such as
	 * Docker...)
	 * We do not want to hold a controlling terminal since:
	 * - we do not need one as an init process,
	 * - we would not want one of our children / services requesting a
	 *   controlling tty to fail getting one.
	 * Indeed there is only one single session / foreground process group
	 * that can hold a given terminal as a controlling tty.
	 */
	fd = open(ctermid(NULL), O_RDONLY | O_NONBLOCK);
	if (fd >= 0)
		/* Do drop current controlling tty. See ioctl_tty(2). */
		ioctl(fd, TIOCNOTTY);
	else
		fd = cons_fd;

	/*
	 * Now close all remaining file descriptors left open except standard
	 * I/Os and fslog related file descriptor.
	 */
	ufd_close_fds(STDERR_FILENO + 2, ~(0U));

	tinit_debug("standard I/Os initialized.");

	return 0;
}

static void
tinit_poll(struct upoll * poller)
{
	int ret;

	do {
#warning TODO: ensure EPOLLWAKEUP does not need to be handled, see epoll(7)
		ret = upoll_process_with_timers(poller);
	} while (ret != -ESHUTDOWN);
}

static int
tinit_loop(void)
{
	struct upoll         poll;
	struct tinit_sigchan sigs;
	struct tinit_srv     srv;
	int                  ret;

	ret = upoll_open(&poll, 2);
	if (ret) {
		tinit_err("poller: cannot initialize: %s (%d).",
		          strerror(-ret),
		          -ret);
		return ret;
	}

	ret = tinit_sigchan_open(&sigs);
	if (ret)
		goto close_poll;

	ret = tinit_target_start(CONFIG_TINIT_SYSCONFDIR,
	                         tinit_boot_target,
	                         &sigs,
	                         &poll);
	if (ret)
		goto close_sigs;

	tinit_srv_open(&srv, TINIT_SOCK_PATH, &poll);

	tinit_poll(&poll);

	switch (tinit_sigchan_get_signo(&sigs)) {
	case SIGTERM:
		tinit_notice("reboot requested.");
		ret = RB_AUTOBOOT;
		break;

	case SIGUSR1:
		tinit_notice("halt requested.");
		ret = RB_HALT_SYSTEM;
		break;

	case SIGUSR2:
	case SIGPWR:
		tinit_notice("power off requested.");
		ret = RB_POWER_OFF;
		break;

	default:
		assert(0);
	}

	tinit_srv_close(&srv, &poll);

	tinit_target_stop(&sigs);

	tinit_poll(&poll);

	goto close_sigs;

close_sigs:
	tinit_sigchan_close(&sigs);
close_poll:
	upoll_close(&poll);

	return ret;
}

#if defined(CONFIG_TINIT_DEBUG)

#include <dirent.h>

static void
tinit_show_pids(void)
{
	DIR * dir;

	dir = opendir("/proc");
	if (!dir)
		return;

	tinit_debug("processes left:");

	while (true) {
		const struct dirent * ent;
		char *                err;
		long                  pid;
		char                  comm[TINIT_COMM_MAX];

		ent = readdir(dir);
		if (!ent)
			break;

		if (ent->d_type != DT_DIR)
			continue;

		pid = strtol(ent->d_name, &err, 10);
		if (*err)
			continue;
		if ((pid <= 1) || (pid > INT_MAX))
			continue;

		if (tinit_load_comm_bypid(pid, comm))
			continue;

		tinit_debug("    %s[%d]", comm, (int)pid);
	}

	closedir(dir);
}

#else  /* !defined(CONFIG_TINIT_DEBUG) */

static inline void tinit_show_pids(void) { }

#endif /* defined(CONFIG_TINIT_DEBUG) */

static void
tinit_killall(void)
{
	siginfo_t info;

	tinit_show_pids();

	/* Kill all remaining processes (except pid 1). */
	kill(-1, SIGKILL);

	while (!waitid(P_ALL, 0, &info, WEXITED))
		tinit_debug("killed PID %d.", info.si_pid);

	tinit_info("killed all processes left.");
}

static void __noreturn __nothrow
tinit_shutdown(int howto)
{
	const char * msg;
	pid_t        pid;

	fflush(NULL);
	sync();

	/* Kill all remaining processes (except pid 1). */
	tinit_killall();

	tinit_prefini_logs();

	mnt_umount_all(MNT_FORCE);

	switch (howto) {
	case RB_AUTOBOOT:
		msg = "rebooting";
		break;
	case RB_HALT_SYSTEM:
		msg = "halting";
		break;
	case RB_POWER_OFF:
		msg = "powering off";
		break;
	default:
		assert(0);
	}

	tinit_notice("%s...", msg);
	tinit_postfini_logs();
	fflush(NULL);
	sync();

	/*
	 * FIXME: Should we perform a sleep(1) here to let I/Os travel to mass
	 *        storage ??
	 */

	/*
	 * We have to fork here, since the kernel calls do_exit(EXIT_SUCCESS)
	 * which can cause the machine to panic when the init process exits...
	 * See <linux>/kernel/reboot.c
	 */
	pid = vfork();
	if (!pid) {
		/* child */
		reboot(howto);
		_exit(EXIT_SUCCESS);
	}

	while (true)
		sleep(UINT_MAX);
}

int
main(int argc, char * const argv[])
{
	const char *        msg;
	int                 ret;
	struct tinit_repo * repo;

	if (getpid() != 1) {
		fprintf(stderr,
		        "%s: must be run as PID 1, exiting.\n",
		        program_invocation_short_name);
		return EXIT_FAILURE;
	}

	umask(0077);

	tinit_preinit_logs();
	tinit_parse_cmdln(argc, argv);

	ret = upath_chdir("/");
	if (ret) {
		msg = "cannot setup initial filesystems";
		goto err;
	}

	init_signals();

	ret = mnt_mount_all();
	if (ret) {
		msg = "cannot setup initial filesystems";
		goto err;
	}

	/*
	 * MUST be done after pseudo filesystems are mounted since fslog
	 * redirects into a file that should be stored under one of them.
	 * See CONFIG_TINIT_FSLOG_PATH definition.
	 */
	tinit_postinit_logs();

	ret = init_stdios();
	if (ret) {
		msg = "cannot setup initial standard I/Os";
		goto err;
	}

	ret = init_environ();
	if (ret) {
		msg = "cannot setup initial environment";
		goto err;
	}

	repo = tinit_repo_get();

	ret = tinit_repo_load(repo);
	if (ret) {
		msg = "cannot load services";
		goto clear;
	}

	ret = tinit_loop();
	if (ret < 0) {
		msg = "cannot run services loop";
		goto clear;
	}

	tinit_repo_clear(repo);

	tinit_shutdown(ret);

	unreachable();

clear:
	tinit_repo_clear(repo);
err:
	tinit_crit("%s: %s (%d).", msg, strerror(-ret), -ret);
	tinit_shutdown(RB_AUTOBOOT);

	unreachable();

	return EXIT_FAILURE;
}
