#include "srv.h"
#include "proto.h"
#include "repo.h"
#include "target.h"
#include "svc.h"
#include "conf.h"
#include "log.h"
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <fnmatch.h>
#include <sys/stat.h>

#define TINIT_SRV_SEND_BUFF_NR (16U)

/******************************************************************************
 * Server side protocol payload handling
 ******************************************************************************/

static ssize_t
tinit_srv_parse_request(const struct unsk_dgram_buff * buff,
                        enum tinit_msg_type *          type,
                        char *                         pattern)
{
	const struct tinit_request_msg * msg = (struct tinit_request_msg *)
	                                       buff->data;
	size_t                           sz = buff->unsk.bytes - sizeof(*msg);

	if ((sz <= 1) || (sz > TINIT_SVC_PATTERN_MAX))
		return -EPROTO;

	if (msg->type >= TINIT_MSG_TYPE_NR)
		return -EPROTO;

	if ((strnlen(msg->pattern, sz) + 1) != sz)
		return -EPROTO;

	*type = msg->type;
	memcpy(pattern, msg->pattern, sz);

	return sz - 1;
}

static void
tinit_srv_build_reply(struct unsk_dgram_buff * buff, int ret)
{
	assert(buff);
	assert(ret <= 0);
	assert(ret >= -4096);

	struct tinit_reply_head * head = (struct tinit_reply_head *)buff->data;

	assert(head->type < TINIT_MSG_TYPE_NR);

	head->ret = (uint16_t)(-ret);
	buff->unsk.bytes = sizeof(*head);
}

static void
tinit_srv_setup_status_reply(struct unsk_dgram_buff * buff)
{
	assert(buff);

	struct tinit_status_reply * msg = (struct tinit_status_reply *)
	                                  buff->data;

	assert(msg->head.type == TINIT_STATUS_MSG_TYPE);

	msg->head.ret = 0;
	buff->unsk.bytes = sizeof(*msg);
}

static int
tinit_srv_append_status_reply(struct unsk_dgram_buff * buff,
                              pid_t                             pid,
                              bool                              on,
                              enum tinit_svc_state              state,
                              const char *             path,
                              size_t                            len)
{
	assert(buff);
	assert((state == TINIT_SVC_STOPPED_STAT) ||
	       (state == TINIT_SVC_STARTING_STAT) ||
	       (state == TINIT_SVC_READY_STAT) ||
	       (state == TINIT_SVC_STOPPING_STAT));
	assert(path);
	assert(path[0]);
	assert(len);
	assert(len < NAME_MAX);

	size_t                      sz;
	struct tinit_status_data *  data;
	struct tinit_status_reply * msg = (struct tinit_status_reply *)
	                                  buff->data;

	assert(!msg->head.ret);
	assert(buff->unsk.bytes >= sizeof(*msg));
	assert(buff->unsk.bytes <= TINIT_MSG_SIZE_MAX);

	sz = stroll_round_upper(buff->unsk.bytes, sizeof(*data));
	data = (struct tinit_status_data *)&buff->data[sz];
	sz += sizeof(*data) + len + 1;
	if (sz > TINIT_MSG_SIZE_MAX) {
		msg->head.ret = (uint16_t)ENOSPC;
		buff->unsk.bytes = sizeof(msg->head);

		return -ENOSPC;
	}

	data->pid = pid;
	data->adm_state = (uint8_t)on;
	data->run_state = (uint8_t)state;
	memcpy(data->conf_path, path, len + 1);

	buff->unsk.bytes = sz;

	return 0;
}

/******************************************************************************
 * Init services related server side logic handling.
 ******************************************************************************/

static int
tinit_srv_request_status(struct unsk_dgram_buff * buff,
                         const char *             pattern)
{
	const struct tinit_repo * repo;
	const struct svc *        svc;
	int                       ret = 0;
	unsigned int              cnt = 0;

	tinit_srv_setup_status_reply(buff);

	repo = tinit_repo_get();
	tinit_repo_foreach(repo, svc) {
		const struct conf_svc * conf = svc->conf;
		const char *            path;

		ret = fnmatch(pattern,
		              conf_get_name(conf),
		              FNM_NOESCAPE | FNM_PERIOD | FNM_EXTMATCH);
		if (ret == FNM_NOMATCH)
			continue;

		if (ret) {
			tinit_srv_build_reply(buff, -EINVAL);
			return 0;
		}

		path = conf_get_path(svc->conf);
		ret = tinit_srv_append_status_reply(buff,
		                                    svc->child,
		                                    svc_is_on(svc),
		                                    svc->state,
		                                    path,
		                                    strlen(path));
		if (ret)
			return 0;

		cnt++;
	}

	if (!cnt)
		tinit_srv_build_reply(buff, -ENOENT);

	return 0;
}

static int
tinit_srv_request_start(struct unsk_dgram_buff * buff,
                        const char *             name,
                        size_t                            len)
{
	struct svc * svc;
	int          ret;

	ret = tinit_check_svc_name(name, len);
	if (ret)
		goto reply;

	svc = tinit_repo_search_byname(tinit_repo_get(), name);
	if (svc) {
		switch (svc->state) {
		case TINIT_SVC_STARTING_STAT:
		case TINIT_SVC_READY_STAT:
			break;

		default:
			svc_start(svc);
		}

		ret = 0;
	}
	else
		ret = -ENOENT;

reply:
	tinit_srv_build_reply(buff, ret);

	return 0;
}

static int
tinit_srv_request_stop(struct unsk_dgram_buff * buff,
                       const char *             name,
                       size_t                            len)
{
	struct svc * svc;
	int          ret;

	ret = tinit_check_svc_name(name, len);
	if (ret)
		goto reply;

	svc = tinit_repo_search_byname(tinit_repo_get(), name);
	if (svc) {
		switch (svc->state) {
		case TINIT_SVC_STOPPED_STAT:
		case TINIT_SVC_STOPPING_STAT:
			break;

		default:
			svc_stop(svc);
		}

		ret = 0;
	}
	else
		ret = -ENOENT;

reply:
	tinit_srv_build_reply(buff, ret);

	return 0;
}

static int
tinit_srv_request_restart(struct unsk_dgram_buff * buff,
                          const char *             name,
                          size_t                   len)
{
#warning FIXME: implement me
	tinit_srv_build_reply(buff, 0);

	return 0;
}

static int
tinit_srv_request_reload(struct unsk_dgram_buff * buff,
                         const char *             name,
                         size_t                            len)
{
	struct svc * svc;
	int          ret;

	ret = tinit_check_svc_name(name, len);
	if (ret)
		goto reply;

	svc = tinit_repo_search_byname(tinit_repo_get(), name);
	if (svc) {
		switch (svc->state) {
		case TINIT_SVC_STOPPED_STAT:
		case TINIT_SVC_STOPPING_STAT:
			svc_start(svc);
			break;

		case TINIT_SVC_STARTING_STAT:
			break;

		case TINIT_SVC_READY_STAT:
			svc_reload(svc);
			break;

		default:
			assert(0);
		}

		ret = 0;
	}
	else
		ret = -ENOENT;

reply:
	tinit_srv_build_reply(buff, ret);

	return 0;
}

static int
tinit_srv_request_switch(struct unsk_dgram_buff * buff,
                         const char *             name,
                         size_t                   len)
{
	int ret;

	ret = tinit_check_svc_name(name, len);
	if (!ret)
		ret = tinit_target_switch(CONFIG_TINIT_SYSCONFDIR, name);

	tinit_srv_build_reply(buff, ret);

	return 0;
}

/******************************************************************************
 * Server side transport handling
 ******************************************************************************/

static struct tinit_srv *
tinit_srv_from_worker(const struct upoll_worker * worker)
{
	return containerof(unsk_async_svc_from_worker(worker),
	                   struct tinit_srv,
	                   unsk);
}

static bool
tinit_srv_are_creds_ok(const struct ucred * creds)
{
	if (!creds->uid || (creds->gid == CONFIG_TINIT_GID))
		return true;

	return false;
}

static int
tinit_srv_recv(const struct tinit_srv * srv,
               struct unsk_dgram_buff * buff)
{
	struct ucred creds;
	int          ret;
	const char * msg;

	ret = unsk_dgram_async_svc_recv(&srv->unsk,
	                                buff,
	                                TINIT_MSG_SIZE_MAX,
	                                &creds,
	                                0);
	switch (ret) {
	case 0:
		break;

	case -EAGAIN:
	case -EINTR:
	case -ENOMEM:
		return ret;

	case -EADDRNOTAVAIL:
		msg = "invalid client address";
		goto err;

	case -EMSGSIZE:
		msg = "client datagram truncated";
		goto err;

	case -EPROTO:
		msg = "missing client credentials";
		goto err;

	default:
		assert(0);
	}

	if (!tinit_srv_are_creds_ok(&creds)) {
		ret = -EACCES;
		msg = "client credentials rejected";
		goto err;
	}

	return 0;

err:
	tinit_info("receive request: %s.", msg);

	return ret;
}

static int
tinit_srv_process_request(struct tinit_srv *       srv,
                          struct unsk_dgram_buff * buff)
{
	enum tinit_msg_type type;
	ssize_t             ret;

	ret = tinit_srv_parse_request(buff, &type, srv->pattern);
	if (ret < 0) {
		tinit_debug("parse request: %s (%zd).", strerror(-ret), -ret);

		return ret;
	}

	switch (type) {
	case TINIT_STATUS_MSG_TYPE:
		ret = tinit_srv_request_status(buff, srv->pattern);
		break;

	case TINIT_START_MSG_TYPE:
		ret = tinit_srv_request_start(buff, srv->pattern, ret);
		break;

	case TINIT_STOP_MSG_TYPE:
		ret = tinit_srv_request_stop(buff, srv->pattern, ret);
		break;

	case TINIT_RESTART_MSG_TYPE:
		ret = tinit_srv_request_restart(buff, srv->pattern, ret);
		break;

	case TINIT_RELOAD_MSG_TYPE:
		ret = tinit_srv_request_reload(buff, srv->pattern, ret);
		break;

	case TINIT_SWITCH_MSG_TYPE:
		ret = tinit_srv_request_switch(buff, srv->pattern, ret);
		break;

	default:
		assert(0);
	}

	return ret;
}

static int
tinit_srv_handle_requests(struct tinit_srv * srv)
{
	assert(srv);

	while (unsk_buffq_has_free(&srv->buffq)) {
		struct unsk_dgram_buff * buff;
		int                      ret;

		buff = unsk_dgram_buffq_dqueue_free(&srv->buffq);

		ret = tinit_srv_recv(srv, buff);
		if (!ret) {
			ret = tinit_srv_process_request(srv, buff);
			if (!ret) {
				unsk_dgram_buffq_nqueue_busy(&srv->buffq, buff);
				continue;
			}
		}

		unsk_dgram_buffq_release(&srv->buffq, buff);

		if (ret == -EAGAIN)
			return 0;
		else if ((ret == -EINTR) || (ret == -ENOMEM))
			return ret;
	}

	return 0;
}

static int
tinit_srv_send(const struct tinit_srv *       srv,
               const struct unsk_dgram_buff * buff,
               int                                     flags)
{
	int ret;

	ret = unsk_dgram_async_svc_send(&srv->unsk, buff, flags);

	switch (ret) {
	case 0:
	case -EAGAIN:
	case -EINTR:
	case -ENOMEM:
		break;

	case -ECONNREFUSED:
		tinit_info("send reply: client connection refused.");
		return -ECONNREFUSED;

	default:
		assert(0);
	}

	return ret;
}

static int
tinit_srv_handle_replies(struct tinit_srv * srv)
{
	while (unsk_buffq_has_busy(&srv->buffq)) {
		struct unsk_dgram_buff * buff;
		int                      ret;

		buff = unsk_dgram_buffq_dqueue_busy(&srv->buffq);

		ret = tinit_srv_send(srv, buff, 0);

		switch (ret) {
		case 0:
		case -ECONNREFUSED:
			unsk_dgram_buffq_release(&srv->buffq, buff);
			break;

		case -EAGAIN:
			unsk_dgram_buffq_requeue_busy(&srv->buffq, buff);
			upoll_enable_watch(&srv->unsk.work, EPOLLOUT);
			return 0;

		case -EINTR:
			unsk_dgram_buffq_requeue_busy(&srv->buffq, buff);
			return -EINTR;

		case -ENOMEM:
			unsk_dgram_buffq_release(&srv->buffq, buff);
			return -ENOMEM;

		default:
			assert(0);
		}
	}

	upoll_disable_watch(&srv->unsk.work, EPOLLOUT);

	return 0;
}

static int
tinit_srv_dispatch(struct upoll_worker * worker,
                   uint32_t              state,
                   const struct upoll *  poller)
{
	assert(worker);
	assert(state);
	assert(!(state & EPOLLRDHUP));
	assert(!(state & EPOLLPRI));
	assert(poller);

#warning FIXME: implement EPOLLERR and EPOLLHUP support ?
	assert(!(state & EPOLLERR));
	assert(!(state & EPOLLHUP));

	int                ret = 0;
	struct tinit_srv * srv = tinit_srv_from_worker(worker);

	if (state & EPOLLIN) {
		/* Kernel has input data available. */
		ret = tinit_srv_handle_requests(srv);
		if (ret)
			return ret;
	}

	ret = tinit_srv_handle_replies(srv);

	unsk_async_svc_apply_watch(&srv->unsk, poller);

	return ret;
}

int
tinit_srv_open(struct tinit_srv *   srv,
               const char *         path,
               const struct upoll * poller)
{
	assert(srv);

	int    err;
	mode_t msk;

	srv->pattern = NULL;

	err = unsk_dgram_buffq_init(&srv->buffq,
	                            TINIT_MSG_SIZE_MAX,
	                            TINIT_SRV_SEND_BUFF_NR);
	if (err) {
		tinit_err("server: cannot initialize buffer queue: %s (%d).",
		          strerror(-err),
		          -err);
		return err;
	}

	srv->pattern = malloc(TINIT_SVC_PATTERN_MAX);
	if (!srv->pattern) {
		err = -errno;

		tinit_err("server: cannot allocate pattern space: %s (%d).",
		          strerror(-err),
		          -err);
		goto fini;
	}

	msk = umask(~(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP));
	err = unsk_dgram_async_svc_open(&srv->unsk,
	                                path,
	                                SOCK_CLOEXEC,
	                                poller,
	                                EPOLLIN,
	                                tinit_srv_dispatch);
	umask(msk);
	if (err) {
		tinit_err("server: cannot open socket: '%s': %s (%d).",
		          path,
		          strerror(-err),
		          -err);
		goto free;
	}

	tinit_debug("server: opened.");

	return 0;

free:
	free(srv->pattern);
	srv->pattern = NULL;
fini:
	unsk_buffq_fini(&srv->buffq);

	return err;
}

void
tinit_srv_close(struct tinit_srv *   srv,
                const struct upoll * poller)
{
	assert(srv);

	int err;

	err = unsk_dgram_async_svc_close(&srv->unsk, poller);
	if (err)
		tinit_warn("cannot close server socket: %s (%d).",
		           strerror(-err),
		           -err);

	free(srv->pattern);
	srv->pattern = NULL;

	unsk_buffq_fini(&srv->buffq);
}
