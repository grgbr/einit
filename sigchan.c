#include "sigchan.h"
#include "svc.h"
#include "repo.h"
#include "log.h"
#include <utils/signal.h>
#include <sys/types.h>
#include <sys/wait.h>

/*
 * Enough entries to hold the following regular signals:
 * SIGCHLD, SIGTERM, SIGUSR1, SIGUSR2, SIGPWR.
 */
#define TINIT_SIGNAL_NR (5U)

#if defined(CONFIG_TINIT_DEBUG)

#include "conf.h"

static void
tinit_sigchan_log_info(const siginfo_t * info, const struct svc * svc)
{
	const char * svc_name;

	if (svc)
		svc_name = conf_get_name(svc->conf);
	else
		svc_name = "unknown";

	switch (info->si_code) {
	case CLD_EXITED:
		tinit_debug("%s[%d]: terminated with %d exit status.",
		            svc_name,
		            info->si_pid,
		            info->si_status);
		break;

	case CLD_KILLED:
		tinit_debug("%s[%d]: killed by '%s' signal (%d).",
		            svc_name,
		            info->si_pid,
		            strsignal(info->si_status),
		            info->si_status);
		break;

	case CLD_DUMPED:
		tinit_debug("%s[%d]: core dumped with '%s' signal (%d).",
		            svc_name,
		            info->si_pid,
		            strsignal(info->si_status),
		            info->si_status);
		break;

	case CLD_TRAPPED:
		tinit_debug("%s[%d]: has trapped.", svc_name, info->si_pid);
		break;

	case CLD_STOPPED:
		tinit_debug("%s[%d]: has stopped "
		            "as a result of '%s' signal (%d).",
		            svc_name,
		            info->si_pid,
		            strsignal(info->si_status),
		            info->si_status);
		break;

	case CLD_CONTINUED:
		tinit_debug("%s[%d]: is continuing "
		            "as a result of '%s' signal (%d).",
		            svc_name,
		            info->si_pid,
		            strsignal(info->si_status),
		            info->si_status);
		break;

	default:
		assert(0);
	}
}

#else  /* !defined(CONFIG_TINIT_DEBUG) */

static inline void
tinit_sigchan_log_info(const siginfo_t *  info __unused,
                       const struct svc * svc __unused)
{

}

#endif /* defined(CONFIG_TINIT_DEBUG) */

static struct tinit_sigchan *
tinit_sigchan_from_worker(const struct upoll_worker * worker)
{
	return containerof(worker, struct tinit_sigchan, work);
}

static unsigned int
tinit_sigchan_handle_sigchld(const struct tinit_repo * repo)
{
	assert(repo);

	unsigned int cnt = 0;

	while (true) {
		int          err;
		siginfo_t    info;
		struct svc * svc;

		/*
		 * As Linux conforms to POSIX.1-2008 Technical Corrigendum 1
		 * (2013), with the WNOHANG option, there is no need to zero out
		 * si_pid field to detect cases where there is no children in a
		 * waitable state.  waitid() will have zero'ed out si_pid and
		 * si_signo fields of the siginfo_t structure in this case.
		 * See section waitid(2) man page for more infos.
		 */
		err = waitid(P_ALL, 0, &info, WNOHANG | WEXITED);
		if (err) {
			/* No more children in waitable state. */
			assert(errno == ECHILD);

			return cnt;
		}

		/* For SIGCHLD, valid siginfo_t fields are:
		 * - si_pid: child process ID,
		 * - si_uid: child's real user ID,
		 * - si_code:
		 *   - CLD_EXITED: child has exited,
		 *   - CLD_KILLED: child was killed,
		 *   - CLD_DUMPED: child terminated abnormally (with coredump),
		 *   - CLD_TRAPPED: traced child has trapped,
		 *   - CLD_STOPPED: child has stopped,
		 *   - CLD_CONTINUED: stopped child has continued,
		 * - si_status:
		 *   - child process exit status if si_code is CLD_EXITED,
		 *   - or signal number that caused the child process to change
		 *     state,
		 * - si_utime: user CPU time used by the child process,
		 * - si_stime: system CPU time used by the child process.
		 *
		 * See sigaction(2) man pages for more infos.
		 */
		if (!info.si_pid || !info.si_signo)
			/* No more child in waitable state. */
			return cnt;

		assert(info.si_pid);
		assert(info.si_signo == SIGCHLD);

		svc = tinit_repo_search_bypid(repo, info.si_pid);

		tinit_sigchan_log_info(&info, svc);

		if (!svc)
			continue;

		switch (info.si_code) {
		case CLD_EXITED:
			svc_handle_evts(svc, SVC_EXIT_EVT, info.si_status);
			break;

		case CLD_KILLED:
		case CLD_DUMPED:
			svc_handle_evts(svc, SVC_EXIT_EVT, -info.si_status);
			break;

		default:
			/*
			 * We should never find other values into si_code field
			 * since:
			 * - we do not ptrace() child processes (hence, no
			 *   expected CLD_TRAPPED value),
			 * - init_signals() installs SIGCHLD handler with
			 *   SA_NOCLDSTOP flag enabled (hence, no expected
			 *   CLD_STOPPED neither CLD_CONTINUED values).
			 */
			assert(0);
		}

		if (svc->state == TINIT_SVC_STOPPED_STAT)
			cnt++;
	}
}

static int
tinit_sigchan_dispatch_started(struct upoll_worker * worker,
                               uint32_t              state,
                               const struct upoll *  poller)
{
	assert(worker);
	assert(state);
	assert(!(state & EPOLLOUT));
	assert(!(state & EPOLLRDHUP));
	assert(!(state & EPOLLPRI));
	assert(!(state & EPOLLHUP));
	assert(!(state & EPOLLERR));
	assert(state & EPOLLIN);
	assert(poller);

	struct tinit_sigchan *  chan = tinit_sigchan_from_worker(worker);
	struct signalfd_siginfo infos[TINIT_SIGNAL_NR];
	int                     ret;
	struct tinit_repo *     repo;
	unsigned int            cnt;
	unsigned int            i;

	ret = usig_read_fd(chan->fd, infos, array_nr(infos));
	assert(ret);
	if (ret < 0)
		return (ret == -EAGAIN) ? 0 : ret;

	repo = tinit_repo_get();

	cnt = ret;
	ret = 0;
	chan->signo = 0;

	for (i = 0; i < cnt; i++) {
		const struct signalfd_siginfo * info = &infos[i];

		switch (info->ssi_signo) {
		case SIGCHLD:
			tinit_sigchan_handle_sigchld(repo);
			break;

		case SIGTERM:
		case SIGUSR1:
		case SIGUSR2:
		case SIGPWR:
			if ((info->ssi_code != SI_USER) &&
			    (info->ssi_code != SI_QUEUE)) {
				tinit_debug("signal channel: "
				            "unexpected '%s' signal (%d) "
				            "received from PID %d.",
				            strsignal(info->ssi_signo),
				            info->ssi_signo,
				            info->ssi_pid);
				break;
			}

			/* Tell the caller we were requested to shutdown. */
			if (!chan->signo) {
				chan->signo = info->ssi_signo;
				ret = -ESHUTDOWN;
			}
			break;

		default:
			assert(0);
		}
	}

	return ret;
}

int
tinit_sigchan_start(struct tinit_sigchan *        chan,
                    const struct upoll * poller)
{
	assert(chan);

	int err;

	chan->work.dispatch = tinit_sigchan_dispatch_started;
	err = upoll_register(poller, chan->fd, EPOLLIN, &chan->work);
	if (err) {
		tinit_err("signal: cannot start channel: %s (%d).",
		          strerror(err),
		          err);
		return err;
	}

	tinit_debug("signal: channel started.");

	return 0;
}

static int
tinit_sigchan_dispatch_stopping(struct upoll_worker * worker,
                                uint32_t              state,
                                const struct upoll *  poller)
{
	assert(worker);
	assert(state);
	assert(!(state & EPOLLOUT));
	assert(!(state & EPOLLRDHUP));
	assert(!(state & EPOLLPRI));
	assert(!(state & EPOLLHUP));
	assert(!(state & EPOLLERR));
	assert(state & EPOLLIN);
	assert(poller);

	struct tinit_sigchan *  chan = tinit_sigchan_from_worker(worker);
	struct signalfd_siginfo infos[TINIT_SIGNAL_NR];
	int                     ret;
	struct tinit_repo *     repo;
	unsigned int            i;

	assert(chan->cnt);

	ret = usig_read_fd(chan->fd, infos, array_nr(infos));
	assert(ret);
	if (ret < 0)
		return (ret == -EAGAIN) ? 0 : ret;

	repo = tinit_repo_get();

	for (i = 0; i < (unsigned int)ret; i++) {
		const struct signalfd_siginfo * info = &infos[i];

		if (info->ssi_signo == SIGCHLD) {
			unsigned int cnt;

			cnt = tinit_sigchan_handle_sigchld(repo);
			assert(cnt <= chan->cnt);

			chan->cnt -= cnt;
			if (!chan->cnt)
				goto closed;
		}
	}

	return 0;

closed:
	upoll_unregister(poller, chan->fd);

	return -ESHUTDOWN;
}

void
tinit_sigchan_stop(struct tinit_sigchan * chan, unsigned int cnt)
{
	assert(chan);

	chan->work.dispatch = tinit_sigchan_dispatch_stopping;
	chan->cnt = cnt;

	tinit_debug("signal: stopping channel...");
}

int
tinit_sigchan_open(struct tinit_sigchan * chan)
{
	assert(chan);

	sigset_t msk = sig_empty_msk;
	int      fd;

	usig_addset(&msk, SIGTERM);
	usig_addset(&msk, SIGUSR1);
	usig_addset(&msk, SIGUSR2);
	usig_addset(&msk, SIGPWR);
	usig_addset(&msk, SIGCHLD);

	fd = usig_open_fd(&msk, SFD_NONBLOCK | SFD_CLOEXEC);
	if (fd < 0) {
		tinit_err("signal: cannot open channel: %s (%d).",
		          strerror(errno),
		          errno);
		return -errno;
	}

	chan->fd = fd;

	tinit_debug("signal: channel opened.");

	return 0;
}

void
tinit_sigchan_close(const struct tinit_sigchan * chan)
{
	usig_close_fd(chan->fd);
}
