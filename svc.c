#include "svc.h"
#include "conf.h"
#include "notif.h"
#include "mnt.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sysexits.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/epoll.h>

static void svc_handle_on_evts(struct svc * svc, enum svc_evt evt, int status);

static void svc_handle_off_evts(struct svc * svc, enum svc_evt evt, int status);

static void svc_handle_on_notif(struct svc *       svc,
                                const struct svc * src);

static void svc_handle_off_notif(struct svc *       svc,
                                 const struct svc * src);

/*
 * svc_handle_notif() - Handle source service state change notifications.
 *
 * @svc: the notification observer service
 * @src: the notifying (source) service
 *
 * Called while the @src service is notifying @svc that it has just changed
 * its internal state.
 */
static void
svc_handle_notif(struct svc * svc, const struct svc * src)
{
	assert(svc);
	assert(svc->handle_notif);
	assert(src);

	svc->handle_notif(svc, src);
}

static void
svc_mark_stopped(struct svc * svc)
{
	const struct notif * obs;

	svc->child = -1;
	svc->state = TINIT_SVC_STOPPED_STAT;

	tinit_info("%s: service stopped.", conf_get_name(svc->conf));

	notif_foreach(&svc->stopon_obsrv, obs) {
		assert(notif_get_src(obs) == svc);

		svc_handle_notif(notif_get_sink(obs), svc);
	}
}

static void
svc_mark_ready(struct svc * svc)
{
	const struct notif * obs;

	svc->state = TINIT_SVC_READY_STAT;

	tinit_info("%s: service ready.", conf_get_name(svc->conf));

	notif_foreach(&svc->starton_obsrv, obs) {
		assert(notif_get_src(obs) == svc);

		svc_handle_notif(notif_get_sink(obs), svc);
	}
}

static int
svc_reopen_stdin(const char * path)
{
	assert(path);
	assert(path[0]);
	assert(strnlen(path, PATH_MAX) < PATH_MAX);
	assert(!strncmp(path, "/dev/", sizeof("/dev/") - 1));

	struct stat st;
	int         ret;

	close(STDIN_FILENO);

	ret = sys_open_stdio(path, O_RDWR | O_NOATIME | O_NOFOLLOW);
	if (ret != STDIN_FILENO)
		return (ret < 0) ? ret : -EBADF;

	ret = sys_fstat(STDIN_FILENO, &st);
	if (ret)
		return ret;

	if (!S_ISCHR(st.st_mode)) {
		tinit_err("%s: pathname not suitable for standard input.",
		          path);
		return -ENOTTY;
	}

	return 0;
}

static int
svc_reopen_stdout(const char * path)
{
	assert(path);
	assert(path[0]);
	assert(strnlen(path, PATH_MAX) < PATH_MAX);

	int ret;

	close(STDOUT_FILENO);

	ret = sys_open_stdio(path,
	                     O_WRONLY | O_APPEND | O_NOATIME | O_NOFOLLOW);
	if (ret != STDOUT_FILENO)
		return (ret < 0) ? ret : -EBADF;

	return 0;
}

static void __noreturn
svc_exec(const struct svc * svc, const char * const * args)
{
	assert(svc);
	assert(args);
	assert(args[0]);

	int                     ret;
	const struct conf_svc * conf = svc->conf;
	char * const *          env;

	/* Create a new session and make ourself the process group leader. */
	ret = setsid();
	assert(ret == getpid());

	/*
	 * As we use the close-on-exec flag at opening time, do not bother
	 * explicitly closing remaining file descriptors.
	 */

	if (conf->stdin)
		if (svc_reopen_stdin(conf->stdin))
			goto exit;

	if (conf->stdout) {
		if (svc_reopen_stdout(conf->stdout))
			goto exit;

		/* Duplicate stderr onto stdout. */
		ret = sys_dup2(STDOUT_FILENO, STDERR_FILENO);
		if (ret < 0)
			goto exit;
	}

	/*
	 * No need to worry about signals disposititon since during an
	 * execve(2), the dispositions of handled signals are reset to the
	 * default; the dispositions of ignored signals are left unchanged.
	 *
	 * However, as signal mask is inherited from parent and left unchanged
	 * by execve(2), unblock all of them to restore system default behavior.
	 */
	ret = sigprocmask(SIG_UNBLOCK, &sig_full_msk, NULL);
	assert(!ret);

	/*
	 * Perform the real exec.
	 *
	 * Warning: we may fetch a NULL env pointer here when no environment was
	 *          specified in the configuration ;
	 *          this will properly work while running onto Linux (and a few
	 *          other UNIX variants) but this is not portable !
	 */
	env = (char * const *)conf_get_env(conf);
	if (execve(args[0], (char * const *)args, env)) {
		int ret = errno;

		assert(ret != EFAULT);
		assert(ret != ENAMETOOLONG);

		tinit_err("%s: cannot execute: %s (%d).",
		          args[0],
		          strerror(ret),
		          ret);
	}

exit:
	_exit(EX_OSERR);
}

static pid_t
svc_spawn(struct svc * svc, const char * const * args, unsigned int tmout)
{
	assert(svc);
	assert(args);
	assert(args[0]);

	pid_t pid;

	pid = vfork();
	if (pid < 0) {
		/* Fork failed. */
		int err = errno;

		assert(err != ENOSYS);

		tinit_err("%s: %s: cannot spawn: %s (%d).",
		          conf_get_name(svc->conf),
		          args[0],
		          strerror(err),
		          err);
		return -err;
	}
	else if (pid > 0) {
		/* Parent: we are blocked till child calls execve() or exits. */
		tinit_debug("%s: %s[%d]: spawned.",
		            conf_get_name(svc->conf),
		            args[0],
		            pid);

		utimer_arm_sec(&svc->timer, tmout);

		return pid;
	}
	else
		/* Child. */
		svc_exec(svc, args);
}

static void
svc_spawn_start_cmd(struct svc * svc)
{
	const char * const * args;
	bool                 mark;

	if (svc->start_cmd < conf_get_start_cmd_nr(svc->conf)) {
		/* Get next start sequence command. */
		args = conf_get_start_cmd(svc->conf, svc->start_cmd);
		mark = false;
	}
	else {
		/* Get command to respawn after start sequence has completed. */
		args = conf_get_daemon(svc->conf);
		mark = true;
	}

	if (args) {
		svc->child = svc_spawn(svc, args, 1U);
		if (svc->child < 0)
			return;
	}
	else
		svc->child = -1;
#warning remove me if tested
#if 0
	else
		/*
		 * No start command to run, no daemon to respawn (this may
		 * happen when service is defined with stop commands only).
		 */
		mark = true;
#endif

	if (mark)
		/* Start sequence is over: switch to ready state. */
		svc_mark_ready(svc);
}

static void
svc_respawn(struct svc * svc)
{
	svc_spawn_start_cmd(svc);
}

static bool
svc_may_start(const struct svc * svc)
{
	assert(svc);
	assert(svc->handle_evts == svc_handle_on_evts);
	assert(svc->handle_notif == svc_handle_on_notif);
	assert(svc->state == TINIT_SVC_STARTING_STAT);

	const struct notif_poll * poll = svc->starton_notif;

	if (poll) {
		unsigned int unready;

		/*
		 * unready may happen to be zero when notifier loops have been
		 * detected at observer registering time.
		 * See svc_register_starton_obsrv().
		 */
		unready = notif_get_poll_cnt(poll);
		if (unready) {
			unsigned int s;
			struct svc * src;

			notif_foreach_sink_poll_src(poll, s, src) {
				if (src->state == TINIT_SVC_READY_STAT)
					unready--;
			}

			if (unready)
				return false;
		}
	}

	return true;
}

static void
svc_expire_on(struct utimer * timer)
{
	struct svc * svc = containerof(timer, struct svc, timer);

	switch (svc->state) {
	case TINIT_SVC_READY_STAT:
		break;

	case TINIT_SVC_STARTING_STAT:
		if (svc->child < 0)
			/* Child does not exist (anymore). Respawn it. */
			svc_respawn(svc);
		break;

	default:
		assert(0);
	}
}

void
svc_start(struct svc * svc)
{
	tinit_info("%s: starting service...", conf_get_name(svc->conf));

	svc->handle_evts = svc_handle_on_evts;
	svc->handle_notif = svc_handle_on_notif;
	svc->state = TINIT_SVC_STARTING_STAT;
	utimer_setup(&svc->timer, svc_expire_on);
	svc->start_cmd = 0;

	if (svc_may_start(svc))
		svc_spawn_start_cmd(svc);
}

static void
svc_spawn_stop_cmd(struct svc * svc)
{
	assert(svc);

	svc->stop_cmd++;

	if ((unsigned int)svc->stop_cmd >= conf_get_stop_cmd_nr(svc->conf)) {
		/* Stop sequence is over: switch to stopped state. */
		svc_mark_stopped(svc);

		return;
	}

	/* Get stop sequence command and spawn it. */
	svc->child = svc_spawn(svc,
	                       conf_get_stop_cmd(svc->conf, svc->stop_cmd),
	                       5U);
}

#warning factorize me with svc_may_start()

static bool
svc_may_stop(const struct svc * svc)
{
	assert(svc);
	assert(svc->handle_evts == svc_handle_off_evts);
	assert(svc->handle_notif == svc_handle_off_notif);
	assert(svc->state == TINIT_SVC_STOPPING_STAT);

	const struct notif_poll * poll = svc->stopon_notif;

	if (poll) {
		unsigned int running;

		/*
		 * running may happen to be zero when notifier loops have been
		 * detected at observer registering time.
		 * See svc_register_stopon_obsrv().
		 */
		running = notif_get_poll_cnt(poll);
		if (running) {
			unsigned int s;
			struct svc * src;

			notif_foreach_sink_poll_src(poll, s, src) {
				if (src->state == TINIT_SVC_STOPPED_STAT)
					running--;
			}

			if (running)
				return false;
		}
	}

	return true;
}

static void
svc_handle_off_evts(struct svc * svc, enum svc_evt evt, int status __unused)
{
	assert(svc);

	switch (svc->state) {
	case TINIT_SVC_STOPPED_STAT:
		switch (evt) {
		case SVC_START_EVT:
			svc_start(svc);
			break;

		case SVC_STOP_EVT:
			break;

		default:
			assert(0);
		}
		break;

	case TINIT_SVC_STOPPING_STAT:
		switch (evt) {
		case SVC_START_EVT:
			svc_start(svc);
			break;

		case SVC_STOP_EVT:
			break;
			
		case SVC_EXIT_EVT:
			svc_spawn_stop_cmd(svc);
			break;

		default:
			assert(0);
		}
		break;

	default:
		assert(0);
	}
}

static void
svc_handle_off_notif(struct svc * svc, const struct svc * src)
{
	assert(svc);
	assert(src);

	switch (svc->state) {
	case TINIT_SVC_STOPPED_STAT:
		return;
	case TINIT_SVC_STOPPING_STAT:
		break;
	default:
		assert(0);
	}

	switch (src->state) {
	case TINIT_SVC_STOPPED_STAT:
		break;
	case TINIT_SVC_STARTING_STAT:
	case TINIT_SVC_READY_STAT:
	case TINIT_SVC_STOPPING_STAT:
		return;
	default:
		assert(0);
	}

	if (svc_may_stop(svc))
		svc_spawn_stop_cmd(svc);
}

static int
svc_kill(const struct svc *svc, int signo)
{
	assert(svc);

	if (svc->child <= 0)
		return -ESRCH;

	if (kill(svc->child, signo)) {
		assert(errno == ESRCH);

		return -ESRCH;
	}

	return 0;
}

static void
svc_expire_off(struct utimer * timer)
{
	struct svc * svc = containerof(timer, struct svc, timer);

	switch (svc->state) {
	case TINIT_SVC_STOPPED_STAT:
		break;

	case TINIT_SVC_STOPPING_STAT:
		/* Child still seems to exist. Kill it roughly ! */
		if (svc_kill(svc, SIGKILL))
			/* Process to kill not found: keep going. */
			svc_spawn_stop_cmd(svc);
		break;

	default:
		assert(0);
	}
}

void
svc_stop(struct svc * svc)
{
	tinit_info("%s: stopping service...", conf_get_name(svc->conf));

	svc->handle_evts = svc_handle_off_evts;
	svc->handle_notif = svc_handle_off_notif;
	svc->state = TINIT_SVC_STOPPING_STAT;
	utimer_setup(&svc->timer, svc_expire_off);
	svc->stop_cmd = -1;

	if (!svc_may_stop(svc))
		return;

	/* Kill current service daemon / process if any. */
	if (!svc_kill(svc, conf_get_stop_sig(svc->conf))) {
		utimer_arm_sec(&svc->timer, 5U);
		return;
	}

	svc_spawn_stop_cmd(svc);
}

void
svc_reload(const struct svc * svc)
{
	assert(svc);
	assert(svc->state == TINIT_SVC_READY_STAT);
	assert(svc->child > 0);

	tinit_info("%s: reloading service...", conf_get_name(svc->conf));

	svc_kill(svc, conf_get_reload_sig(svc->conf));
}

static void
svc_handle_on_evts(struct svc * svc, enum svc_evt evt, int status)
{
	switch (svc->state) {
	case TINIT_SVC_STARTING_STAT:
		switch (evt) {
		case SVC_START_EVT:
			break;

		case SVC_STOP_EVT:
			svc_stop(svc);
			break;

		case SVC_EXIT_EVT:
			if (!status) {
				svc->start_cmd++;
				svc_respawn(svc);
				break;
			}

			if (!utimer_is_armed(&svc->timer)) {
				svc_respawn(svc);
				break;
			}

			svc->child = -1;
			break;

		default:
			assert(0);
		}
		break;

	case TINIT_SVC_READY_STAT:
		switch (evt) {
		case SVC_START_EVT:
			break;

		case SVC_STOP_EVT:
			svc_stop(svc);
			break;

		case SVC_EXIT_EVT:
			if (!utimer_is_armed(&svc->timer)) {
				svc->state = TINIT_SVC_STARTING_STAT;
				svc_respawn(svc);
				break;
			}

			svc->child = -1;
			break;

		default:
			assert(0);
		}
		break;

	default:
		assert(0);
	}
}

static void
svc_handle_on_notif(struct svc * svc, const struct svc * src)
{
	assert(svc);
	assert(src);

	switch (svc->state) {
	case TINIT_SVC_STARTING_STAT:
		break;
	case TINIT_SVC_READY_STAT:
		return;
	default:
		assert(0);
	}

	switch (src->state) {
	case TINIT_SVC_READY_STAT:
		break;
	case TINIT_SVC_STARTING_STAT:
	case TINIT_SVC_STOPPED_STAT:
	case TINIT_SVC_STOPPING_STAT:
		return;
	default:
		assert(0);
	}

	if (svc_may_start(svc))
		svc_spawn_start_cmd(svc);
}

bool
svc_is_on(const struct svc * svc)
{
	return svc->handle_notif == svc_handle_on_notif;
}

static bool
svc_has_starton_notifier(const struct svc * svc, const struct svc * match)
{
	assert(svc);
	assert(match);

	if (svc == match)
		return true;

	if (svc->starton_notif) {
		unsigned int n;
		struct svc * notif;

		notif_foreach_sink_poll_src(svc->starton_notif, n, notif) {
			if (svc_has_starton_notifier(notif, match))
				return true;
		}
	}

	return false;
}

void
svc_register_starton_obsrv(struct svc * svc, struct svc * obsrv)
{
	assert(svc);
	assert(svc->conf);
	assert(obsrv);
	assert(obsrv->conf);
	assert(obsrv->starton_notif);

	if (svc_has_starton_notifier(svc, obsrv)) {
		tinit_err("%s: starton observer service %s: "
		          "notifier loop detected.",
		          conf_get_name(svc->conf),
		          conf_get_name(obsrv->conf));
		return;
	}

	notif_register_poll_sink(obsrv->starton_notif,
	                         &svc->starton_obsrv,
	                         svc);

	tinit_debug("%s: starton observer service %s registered.",
	            conf_get_name(svc->conf),
	            conf_get_name(obsrv->conf));
}

static bool
svc_has_stopon_notifier(const struct svc * svc, const struct svc * match)
{
	assert(svc);
	assert(match);

	if (svc == match)
		return true;

	if (svc->stopon_notif) {
		unsigned int n;
		struct svc * notif;

		notif_foreach_sink_poll_src(svc->stopon_notif, n, notif) {
			if (svc_has_stopon_notifier(notif, match))
				return true;
		}
	}

	return false;
}

#warning factorize me with svc_register_starton_obsrv()
void
svc_register_stopon_obsrv(struct svc * svc, struct svc * obsrv)
{
	assert(svc);
	assert(svc->conf);
	assert(obsrv);
	assert(obsrv->conf);
	assert(obsrv->stopon_notif);

	if (svc_has_stopon_notifier(svc, obsrv)) {
		tinit_err("%s: stopon observer service %s: "
		          "notifier loop detected.",
		          conf_get_name(svc->conf),
		          conf_get_name(obsrv->conf));
		return;
	}

	notif_register_poll_sink(obsrv->stopon_notif,
	                         &svc->stopon_obsrv,
	                         svc);

	tinit_debug("%s: stopon observer service %s registered.",
	            conf_get_name(svc->conf),
	            conf_get_name(obsrv->conf));
}

static int
svc_init_notif_obsrv(struct svc *               svc,
                     struct notif_poll **       notif,
                     struct stroll_dlist_node * obsrv,
                     const struct strarr *      conf)
{
	if (conf) {
		struct notif_poll * poll;

		poll = notif_create_sink_poll(svc, strarr_nr(conf));
		if (!poll)
			return -errno;

		*notif = poll;
	}
	else
		*notif = NULL;

	stroll_dlist_init(obsrv);

	return 0;
}

static void
svc_fini_notif_obsrv(struct notif_poll * notif)
{
	if (notif)
		notif_destroy_sink_poll(notif);
}

static int
svc_init(struct svc * svc, const struct conf_svc * conf)
{
	assert(svc);
	assert(conf);

	int err;

	err = svc_init_notif_obsrv(svc,
	                           &svc->starton_notif,
	                           &svc->starton_obsrv,
	                           conf_get_starton(conf));
	if (err)
		return err;

	err = svc_init_notif_obsrv(svc,
	                           &svc->stopon_notif,
	                           &svc->stopon_obsrv,
	                           conf_get_stopon(conf));
	if (err)
		goto fini;

	svc->handle_evts = svc_handle_off_evts;
	svc->handle_notif = svc_handle_off_notif;
	svc->child = -1;
	svc->state = TINIT_SVC_STOPPED_STAT;
	utimer_init(&svc->timer);
	svc->conf = conf;

	return 0;


fini:
	svc_fini_notif_obsrv(svc->starton_notif);

	return err;
}

struct svc *
svc_create(const struct conf_svc * conf)
{
	assert(conf);

	struct svc * svc;

	svc = malloc(sizeof(*svc));
	if (!svc)
		return NULL;

	svc_init(svc, conf);

	tinit_debug("%s: service created.", conf_get_name(svc->conf));

	return svc;
}

static void
svc_unregister_notif_obsrv(struct stroll_dlist_node * obsrv,
                           struct notif_poll *        notif)
{
	/* Unregister all state changes observers. */
	while (!stroll_dlist_empty(obsrv)) {
		struct notif * notif;

		notif = notif_from_dlist(stroll_dlist_next(obsrv));
		assert(notif);

		notif_unregister_sink(notif);
	}

	svc_fini_notif_obsrv(notif);
}

static void
svc_fini(struct svc * svc)
{
	assert(svc);

	/* Unregister all state changes observers. */
	svc_unregister_notif_obsrv(&svc->starton_obsrv, svc->starton_notif);
	svc_unregister_notif_obsrv(&svc->stopon_obsrv, svc->stopon_notif);

	conf_destroy((struct conf_svc *)svc->conf);
}

void
svc_destroy(struct svc * svc)
{
	tinit_debug("%s: service destroyed.", conf_get_name(svc->conf));

	svc_fini(svc);

	free(svc);
}
