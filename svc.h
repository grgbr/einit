#ifndef _TINIT_SVC_H
#define _TINIT_SVC_H

#include <tinit/tinit.h>
#include <utils/timer.h>
#include <unistd.h>
#include <assert.h>

struct svc;
struct conf_svc;
struct notif_poll;

enum svc_evt {
	SVC_START_EVT,
	SVC_STOP_EVT,
	SVC_EXIT_EVT
};

typedef void (svc_handle_evts_fn)(struct svc * svc,
                                  enum svc_evt evt,
                                  int          status);

typedef void (svc_handle_notif_fn)(struct svc *       svc,
                                   const struct svc * src);

struct svc {
	struct dlist_node       repo;
	svc_handle_evts_fn *    handle_evts;
	pid_t                   child;
	enum tinit_svc_state    state;
	svc_handle_notif_fn *   handle_notif;
	struct utimer           timer;
	unsigned int            start_cmd;
	struct dlist_node       starton_obsrv;
	struct notif_poll *     starton_notif;
	int                     stop_cmd;
	struct dlist_node       stopon_obsrv;
	struct notif_poll *     stopon_notif;
	const struct conf_svc * conf;
};

extern bool
svc_is_on(const struct svc * svc);

static inline void
svc_handle_evts(struct svc * svc, enum svc_evt evt, int status)
{
	assert(svc);
	assert(svc->handle_evts);

	svc->handle_evts(svc, evt, status);
}

/*
 * svc_register_starton_obsrv() - Register to service ready notifications
 * 
 * @svc:   the notifying service to register to
 * @obsrv: the observer service to be notified
 */
extern void
svc_register_starton_obsrv(struct svc * svc, struct svc * obsrv);

/*
 * svc_register_stopon_obsrv() - Register to service stopped notifications
 * 
 * @svc:   the notifying service to register to
 * @obsrv: the observer service to be notified
 */
extern void
svc_register_stopon_obsrv(struct svc * svc, struct svc * obsrv);

extern void
svc_start(struct svc * svc);

extern void
svc_stop(struct svc * svc);

extern void
svc_reload(const struct svc * svc);

extern struct svc *
svc_create(const struct conf_svc * conf);

extern void
svc_destroy(struct svc * svc);

#endif /* _TINIT_SVC_H */
