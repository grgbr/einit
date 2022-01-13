#ifndef _TINIT_H
#define _TINIT_H

#include <tinit/config.h>
#include <utils/unsk.h>

struct conf_svc;
struct tinit_status_reply;
struct elog;

enum tinit_svc_state {
	TINIT_SVC_STOPPED_STAT,
	TINIT_SVC_STARTING_STAT,
	TINIT_SVC_READY_STAT,
	TINIT_SVC_STOPPING_STAT
};

struct tinit_status_data {
	uint32_t pid;
	uint8_t  adm_state;
	uint8_t  run_state;
	char     conf_path[0];
};

struct tinit_status_iter {
	const struct tinit_status_reply * msg;
	const char *                      end;
	struct tinit_status_data *        status;
	size_t                            len;
};

#if !defined(CONFIG_TINIT_ASSERT)

static inline pid_t
tinit_get_status_pid(const struct tinit_status_iter * iter)
{
	return iter->status->pid;
}

static inline bool
tinit_get_status_adm_state(const struct tinit_status_iter * iter)
{
	return !!iter->status->adm_state;
}

static inline enum tinit_svc_state
tinit_get_status_run_state(const struct tinit_status_iter * iter)
{
	return iter->status->run_state;
}

#else /* !defined(CONFIG_TINIT_ASSERT) */

extern pid_t
tinit_get_status_pid(const struct tinit_status_iter * iter);

extern bool
tinit_get_status_adm_state(const struct tinit_status_iter * iter);

extern enum tinit_svc_state
tinit_get_status_run_state(const struct tinit_status_iter * iter);

#endif /* defined(CONFIG_TINIT_ASSERT) */

extern struct conf_svc *
tinit_get_status_conf(const struct tinit_status_iter * iter);

extern int
tinit_step_status(struct tinit_status_iter * iter);

struct tinit_sock {
	struct unsk_clnt unsk;
	uint16_t         seqno;
	char *           reply;
};

extern ssize_t
tinit_parse_svc_name(const char * name);

extern ssize_t
tinit_parse_svc_pattern(const char * pattern);

extern int
tinit_load_status(struct tinit_sock *        sock,
                  const char *               pattern,
                  size_t                     len,
                  struct tinit_status_iter * iter);

extern int
tinit_start_svc(struct tinit_sock * sock,
                const char *        name,
                size_t                       len);

extern int
tinit_stop_svc(struct tinit_sock * sock,
               const char *        name,
               size_t                       len);

extern int
tinit_restart_svc(struct tinit_sock * sock,
                  const char *        name,
                  size_t                       len);

extern int
tinit_reload_svc(struct tinit_sock * sock,
                 const char *        name,
                 size_t                       len);

extern int
tinit_switch_target(struct tinit_sock * sock,
                    const char *        name,
                    size_t                       len);

extern int
tinit_open_sock(struct tinit_sock * sock, uint16_t seqno);

extern void
tinit_close_sock(struct tinit_sock * sock);

extern struct elog * tinit_logger;

static inline void
tinit_setup_logger(struct elog * logger)
{
	tinit_logger = logger;
}

#endif /* _TINIT_H */
