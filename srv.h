#ifndef _TINIT_SRV_H
#define _TINIT_SRV_H

#include "common.h"
#include <utils/unsk.h>

struct tinit_srv {
	struct unsk_async_svc unsk;
	struct unsk_buffq     buffq;
	char *                pattern;
};

extern int
tinit_srv_open(struct tinit_srv *            srv,
               const char *         path,
               const struct upoll * poller);

extern void
tinit_srv_close(struct tinit_srv *   srv,
                const struct upoll * poller);

#endif /* _TINIT_SRV_H */
