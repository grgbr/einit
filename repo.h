#ifndef _TINIT_REPO_H
#define _TINIT_REPO_H

#include "common.h"
#include <utils/dlist.h>

struct svc;

struct tinit_repo {
	struct dlist_node list;
};

#define tinit_repo_foreach(_repo, _svc) \
	dlist_foreach_entry(&(_repo)->list, _svc, repo)

extern struct svc *
tinit_repo_search_byname(
	const struct tinit_repo * repo,
	const char                         name[TINIT_SVC_NAME_MAX]);

extern struct svc *
tinit_repo_search_bypath(const struct tinit_repo * repo,
                         const char                         path[NAME_MAX]);

extern struct svc *
tinit_repo_search_bypid(const struct tinit_repo * repo,
                        pid_t                              pid);

extern int
tinit_repo_load(struct tinit_repo * repo);

#if defined(CONFIG_TINIT_DEBUG)

extern void
tinit_repo_clear(struct tinit_repo * repo);

#else  /* !defined(CONFIG_TINIT_DEBUG) */

static inline void tinit_repo_clear(struct tinit_repo * repo __unused) { }

#endif /* defined(CONFIG_TINIT_DEBUG) */

extern struct tinit_repo tinit_repo_inst;

static inline struct tinit_repo *
tinit_repo_get(void)
{
	return &tinit_repo_inst;
}

#endif /* _TINIT_REPO_H */
