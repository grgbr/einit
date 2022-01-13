#ifndef _TINIT_SIGCHAN_H
#define _TINIT_SIGCHAN_H

#include <utils/poll.h>
#include <assert.h>

struct tinit_sigchan {
	struct upoll_worker work;
	int                 fd;
	int                 signo;
	unsigned int        cnt;
};

static inline int
tinit_sigchan_get_signo(const struct tinit_sigchan * chan)
{
	assert(chan);

	return chan->signo;
}

extern int
tinit_sigchan_start(struct tinit_sigchan *        chan,
                    const struct upoll * poller);

extern void
tinit_sigchan_stop(struct tinit_sigchan * chan, unsigned int cnt);

extern int
tinit_sigchan_open(struct tinit_sigchan * chan);

extern void
tinit_sigchan_close(const struct tinit_sigchan * chan);

#endif /* _TINIT_SIGCHAN_H */
