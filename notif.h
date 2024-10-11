#ifndef _TINIT_NOTIF_H
#define _TINIT_NOTIF_H

#include "common.h"
#include <stroll/dlist.h>
#include <assert.h>

struct svc;

struct notif {
	struct stroll_dlist_node node;
	struct svc *             src;
	struct svc *             sink;
};

#define notif_foreach(_list, _notif) \
	stroll_dlist_foreach_entry(_list, _notif, node)

static inline struct notif * 
notif_from_dlist(struct stroll_dlist_node * node)
{
	return stroll_dlist_entry(node, struct notif, node);
}

static inline struct svc *
notif_get_sink(const struct notif * notif)
{
	assert(notif);
	assert(notif->src);
	assert(notif->sink);

	return notif->sink;
}

static inline struct svc *
notif_get_src(const struct notif * notif)
{
	assert(notif);
	assert(notif->src);
	assert(notif->sink);

	return notif->src;
}

extern void notif_unregister_sink(struct notif * notif);

struct notif_poll {
	unsigned int cnt;
	unsigned int nr;
	struct notif members[];
};

#define notif_foreach_sink_poll_src(_poll, _index, _src) \
	for ((_index) = 0, (_src) = (_poll)->members[(_index)].src; \
	     (_index) < (_poll)->cnt; \
	     (_index)++, (_src) = (_poll)->members[(_index)].src)

static inline unsigned int
notif_get_poll_nr(const struct notif_poll * poll)
{
	assert(poll);
	assert(poll->nr);

	return poll->nr;
}

static inline unsigned int
notif_get_poll_cnt(const struct notif_poll * poll)
{
	assert(poll);
	assert(poll->nr);

	return poll->cnt;
}

extern void
notif_register_poll_sink(struct notif_poll *        poll,
                         struct stroll_dlist_node * sinks,
                         struct svc *               src);

extern void
notif_unregister_poll_sinks(struct notif_poll * poll);

extern struct notif_poll *
notif_create_sink_poll(struct svc * sink, unsigned int nr);

extern void
notif_destroy_sink_poll(struct notif_poll * poll);

#endif /* _TINIT_NOTIF_H */
