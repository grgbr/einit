#include "notif.h"
#include <stdlib.h>

static void
notif_register_sink(struct notif *             notif,
                    struct stroll_dlist_node * sinks,
                    struct svc *               src)
{
	assert(notif);
	assert(!notif->src);
	assert(notif->sink);
	assert(sinks);
	assert(src);

	notif->src = src;

	stroll_dlist_nqueue_back(sinks, &notif->node);
}

void
notif_unregister_sink(struct notif * notif)
{
	assert(notif);
	assert(notif->sink);

	if (notif->src) {
		assert(!stroll_dlist_empty(&notif->node));

		stroll_dlist_remove(&notif->node);
		notif->src = NULL;
	}
}

void
notif_register_poll_sink(struct notif_poll *        poll,
                         struct stroll_dlist_node * sinks,
                         struct svc *               src)
{
	assert(poll);
	assert(poll->nr);
	assert(poll->cnt <= poll->nr);

	struct notif * notif = &poll->members[poll->cnt];

	notif_register_sink(notif, sinks, src);

	poll->cnt++;
}

void
notif_unregister_poll_sinks(struct notif_poll * poll)
{
	assert(poll);
	assert(poll->nr);

	unsigned int m;

	for (m = 0; m < poll->cnt; m++)
		notif_unregister_sink(&poll->members[m]);

	poll->cnt = 0;
}

struct notif_poll *
notif_create_sink_poll(struct svc * sink, unsigned int nr)
{
	assert(sink);
	assert(nr);

	struct notif_poll * poll;
	unsigned int        m;

	poll = malloc(sizeof(*poll) + (nr * sizeof(poll->members[0])));
	if (!poll)
		return NULL;

	for (m = 0; m < nr; m++) {
		struct notif * notif = &poll->members[m];

		notif->src = NULL;
		notif->sink = sink;
	}

	poll->cnt = 0;
	poll->nr = nr;

	return poll;
}

void
notif_destroy_sink_poll(struct notif_poll * poll)
{
	assert(poll);

	notif_unregister_poll_sinks(poll);
	free(poll);
}
