#include "common.h"
#include "conf.h"
#include "proto.h"
#include <utils/path.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fnmatch.h>
#include <errno.h>
#include <assert.h>

ssize_t
tinit_parse_svc_name(const char * name)
{
	assert(name);

	ssize_t len;

	len = upath_validate_path(name, TINIT_SVC_NAME_MAX);
	if (len < 0)
		return len;

	if (tinit_probe_inval_char(name, len))
		return -EINVAL;

	return len;
}

ssize_t
tinit_parse_svc_pattern(const char * pattern)
{
	assert(pattern);

	size_t len;
	int    ret;

	len = strnlen(pattern, TINIT_SVC_PATTERN_MAX);
	if (len >= TINIT_SVC_PATTERN_MAX)
		return -ENAMETOOLONG;
	if (!len)
		return -ENODATA;

	ret = fnmatch(pattern, "", FNM_NOESCAPE | FNM_PERIOD | FNM_EXTMATCH);
	if (ret && (ret != FNM_NOMATCH))
		return -EINVAL;

	return len;
}

#if defined(CONFIG_TINIT_ASSERT)

#define TINIT_ASSERT_STATUS_ITER(_iter) \
	({ \
		const struct tinit_status_iter * __iter = _iter; \
		\
		assert(__iter); \
		assert(__iter->msg); \
		assert(__iter->end); \
		assert(__iter->len); \
		\
		const struct tinit_status_data * __status = __iter->status; \
		\
		assert(__status); \
		assert(strlen(__status->conf_path) == __iter->len); \
		\
		switch ((enum tinit_svc_state)__status->run_state) { \
		case TINIT_SVC_STOPPED_STAT: \
		case TINIT_SVC_STOPPING_STAT: \
			assert(!__status->pid); \
			assert(!__status->adm_state); \
			break; \
		\
		case TINIT_SVC_STARTING_STAT: \
			assert(!__status->pid); \
			assert(__status->adm_state == 1); \
			break; \
		\
		case TINIT_SVC_READY_STAT: \
			assert(__status->pid); \
			assert(__status->adm_state == 1); \
			break; \
		\
		default: \
			assert(0); \
		} \
	 })

pid_t
tinit_get_status_pid(const struct tinit_status_iter * iter)
{
	TINIT_ASSERT_STATUS_ITER(iter);

	return iter->status->pid;
}

bool
tinit_get_status_adm_state(const struct tinit_status_iter * iter)
{
	TINIT_ASSERT_STATUS_ITER(iter);

	return !!iter->status->adm_state;
}

enum tinit_svc_state
tinit_get_status_run_state(const struct tinit_status_iter * iter)
{
	TINIT_ASSERT_STATUS_ITER(iter);

	return iter->status->run_state;
}

#else  /* !defined(CONFIG_TINIT_ASSERT) */

#define TINIT_ASSERT_STATUS_ITER(_iter)

#endif /* defined(CONFIG_TINIT_ASSERT) */

struct conf_svc *
tinit_get_status_conf(const struct tinit_status_iter * iter)
{
	TINIT_ASSERT_STATUS_ITER(iter);

	char *            path;
	struct conf_svc * conf;

	path = malloc(sizeof(CONFIG_TINIT_INCLUDE_DIR) + iter->len + 1);
	if (!path)
		return NULL;

	memcpy(path,
	       CONFIG_TINIT_INCLUDE_DIR,
	       sizeof(CONFIG_TINIT_INCLUDE_DIR) - 1);
	path[sizeof(CONFIG_TINIT_INCLUDE_DIR) - 1] = '/';
	memcpy(&path[sizeof(CONFIG_TINIT_INCLUDE_DIR)],
	       iter->status->conf_path,
	       iter->len + 1);

	conf = conf_create_from_file(path);

	free(path);

	return conf;
}

static ssize_t
tinit_parse_status_data(struct tinit_status_data * status, const char * end)
{
	assert(status);
	assert(end);

	if (&status->conf_path[0] < end) {
		size_t max = umin(end - &status->conf_path[0], NAME_MAX);
		size_t len;

		len = strnlen(&status->conf_path[0], max);
		if (!len || (len >= max))
			return -EPROTO;

		if (!status->adm_state) {
			switch ((enum tinit_svc_state)status->run_state) {
			case TINIT_SVC_STOPPED_STAT:
			case TINIT_SVC_STOPPING_STAT:
				break;

			default:
				return -EPROTO;
			}

			status->pid = 0;
		}
		else if (status->adm_state == 1) {
			switch ((enum tinit_svc_state)status->run_state) {
			case TINIT_SVC_STARTING_STAT:
				status->pid = 0;
				break;

			case TINIT_SVC_READY_STAT:
				if (!status->pid)
					return -EPROTO;
				break;

			default:
				return -EPROTO;
			}
		}
		else
			return -EPROTO;

		return len;
	}

	return -ENOENT;
}

int
tinit_step_status(struct tinit_status_iter * iter)
{
	const struct tinit_status_data * curr = iter->status;
	struct tinit_status_data *       nxt;
	ssize_t                          len;

	nxt = (struct tinit_status_data *)
	      ((char *)curr +
	       uround_upper(sizeof(*curr) + iter->len + 1, sizeof(*curr)));

	len = tinit_parse_status_data(nxt, iter->end);
	if (len < 0)
		return (int)len;

	iter->len = len;
	iter->status = nxt;

	return 0;
}

static size_t
tinit_build_request(char                  buff[TINIT_REQUEST_SIZE_MAX],
                    uint16_t              seqno,
                    enum tinit_msg_type   type,
                    const char * name,
                    size_t                len)
{
	assert(buff);
	assert(type >= 0);
	assert(type < TINIT_MSG_TYPE_NR);
	assert(name);
	assert(*name);
	assert(len < TINIT_SVC_PATTERN_MAX);
	assert(name[len] == '\0');

	struct tinit_request_msg * msg = (struct tinit_request_msg *)buff;

	msg->seq = seqno;
	msg->type = type;
	memcpy(msg->pattern, name, len);
	msg->pattern[len] = '\0';

	return sizeof(*msg) + len + 1;
}

static int
tinit_parse_status_reply(struct tinit_status_iter * iter,
                         const char *               buff,
                         size_t                              size,
                         uint16_t                            seqno)
{
	assert(buff);

	struct tinit_status_reply * msg = (struct tinit_status_reply *)buff;
	const char *                end;
	ssize_t                     len;

	if ((size < sizeof(msg->head)) ||
	    (msg->head.seq != seqno) ||
	    (msg->head.type != TINIT_STATUS_MSG_TYPE))
		return -EPROTO;

	if (msg->head.ret)
		return -((int)msg->head.ret);

	if (size < sizeof(*msg))
	    return -EPROTO;

	end = ((char *)msg) + size;
	len = tinit_parse_status_data(&msg->statuses[0], end);
	if (len < 0)
		return -EPROTO;

	iter->msg = msg;
	iter->end = end;
	iter->status = &msg->statuses[0];
	iter->len = len;

	return 0;
}

int
tinit_load_status(struct tinit_sock *        sock,
                  const char *               pattern,
                  size_t                              len,
                  struct tinit_status_iter * iter)
{
	assert(sock);
	assert(pattern);
	assert(tinit_parse_svc_pattern(pattern) == (ssize_t)len);
	assert(iter);

	char     req[TINIT_REQUEST_SIZE_MAX];
	uint16_t seqno = sock->seqno;
	ssize_t  ret;

	ret = unsk_dgram_clnt_send(&sock->unsk,
	                           req,
	                           tinit_build_request(req,
	                                               seqno,
	                                               TINIT_STATUS_MSG_TYPE,
	                                               pattern,
	                                               len),
	                           0);
	if (ret)
		return ret;

	sock->seqno++;

	ret = unsk_dgram_clnt_recv(&sock->unsk,
	                           sock->reply,
	                           TINIT_MSG_SIZE_MAX,
	                           0);
	if (ret < 0)
		return ret;

	return tinit_parse_status_reply(iter, sock->reply, ret, seqno);
}

static int
tinit_parse_named_reply(const char * buff,
                        size_t                size,
                        uint16_t              seqno,
                        enum tinit_msg_type   type)
{
	assert(buff);
	assert(type >= 0);
	assert(type < TINIT_MSG_TYPE_NR);

	const struct tinit_reply_head * msg = (struct tinit_reply_head *)buff;

	if ((size != sizeof(*msg)) ||
	    (msg->seq != seqno) ||
	    (msg->type != type))
		return -EPROTO;

	return -((int)msg->ret);
}

static int
tinit_named_chat(struct tinit_sock * sock,
                 enum tinit_msg_type          type,
                 const char *        name,
                 size_t                       len)
{
	assert(sock);
	assert(name);
	assert(tinit_parse_svc_name(name) == (ssize_t)len);

	char     req[TINIT_REQUEST_SIZE_MAX];
	uint16_t seqno = sock->seqno;
	ssize_t  ret;

	ret = unsk_dgram_clnt_send(&sock->unsk,
	                           req,
	                           tinit_build_request(req,
	                                               seqno,
	                                               type,
	                                               name,
	                                               len),
	                           0);
	if (ret)
		return ret;

	sock->seqno++;

	ret = unsk_dgram_clnt_recv(&sock->unsk,
	                           sock->reply,
	                           TINIT_MSG_SIZE_MAX,
	                           0);
	if (ret < 0)
		return ret;

	return tinit_parse_named_reply(sock->reply, ret, seqno, type);
}

int
tinit_start_svc(struct tinit_sock * sock,
                const char *        name,
                size_t                       len)
{
	return tinit_named_chat(sock, TINIT_START_MSG_TYPE, name, len);
}

int
tinit_stop_svc(struct tinit_sock * sock,
               const char *        name,
               size_t                       len)
{
	return tinit_named_chat(sock, TINIT_STOP_MSG_TYPE, name, len);
}

int
tinit_restart_svc(struct tinit_sock * sock,
                  const char *        name,
                  size_t                       len)
{
	return tinit_named_chat(sock, TINIT_RESTART_MSG_TYPE, name, len);
}

int
tinit_reload_svc(struct tinit_sock * sock,
                 const char *        name,
                 size_t                       len)
{
	return tinit_named_chat(sock, TINIT_RELOAD_MSG_TYPE, name, len);
}

int
tinit_switch_target(struct tinit_sock * sock,
                    const char *        name,
                    size_t                       len)
{
	return tinit_named_chat(sock, TINIT_SWITCH_MSG_TYPE, name, len);
}

int
tinit_open_sock(struct tinit_sock * sock, uint16_t seqno)
{
	assert(sock);

	int err;

	sock->reply = malloc(TINIT_MSG_SIZE_MAX);
	if (!sock->reply)
		return -errno;

	err = unsk_dgram_clnt_open(&sock->unsk, SOCK_CLOEXEC);
	if (err)
		goto free;

	err = unsk_dgram_clnt_connect(&sock->unsk, TINIT_SOCK_PATH);
	if (err)
		goto close;

	sock->seqno = seqno;

	return 0;

close:
	unsk_clnt_close(&sock->unsk);
free:
	free(sock->reply);

	return err;
}

void
tinit_close_sock(struct tinit_sock * sock)
{
	assert(sock);
	assert(sock->reply);

	unsk_clnt_close(&sock->unsk);
	free(sock->reply);
}
