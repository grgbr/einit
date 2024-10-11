#include "repo.h"
#include "log.h"
#include "svc.h"
#include "conf.h"
#include "notif.h"
#include <assert.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>

#define TINIT_INCLUDE_DIR_LEN \
	(sizeof(CONFIG_TINIT_INCLUDE_DIR) - 1)

#define tinit_repo_err(_err, _fmt, ...) \
	tinit_err("'" CONFIG_TINIT_INCLUDE_DIR "': " _fmt ": %s (%d).", \
	          ## __VA_ARGS__, \
	          strerror(_err), \
	          _err)

struct tinit_repo tinit_repo_inst = {
	.list = STROLL_DLIST_INIT(tinit_repo_inst.list)
};

struct svc *
tinit_repo_search_byname(
	const struct tinit_repo * repo,
	const char                         name[TINIT_SVC_NAME_MAX])
{
	assert(repo);
	assert(name);
	assert(*name);
	assert(strnlen(name, TINIT_SVC_NAME_MAX) < TINIT_SVC_NAME_MAX);

	const struct svc * svc;

	tinit_repo_foreach(repo, svc) {
		if (!strncmp(conf_get_name(svc->conf),
		             name,
		             TINIT_SVC_NAME_MAX))
			return (struct svc *)svc;
	}

	return NULL;
}

struct svc *
tinit_repo_search_bypath(const struct tinit_repo * repo,
                         const char                         path[NAME_MAX])
{
	assert(repo);
	assert(path);
	assert(*path);
	assert(strnlen(path, NAME_MAX) < NAME_MAX);

	const struct svc * svc;

	tinit_repo_foreach(repo, svc) {
		if (!strncmp(conf_get_path(svc->conf), path, NAME_MAX))
			return (struct svc *)svc;
	}

	return NULL;
}

struct svc *
tinit_repo_search_bypid(const struct tinit_repo * repo,
                        pid_t                              pid)
{
	assert(repo);
	assert(pid > 0);

	const struct svc * svc;

	tinit_repo_foreach(repo, svc) {
		if (svc->child == pid)
			return (struct svc *)svc;
	}

	return NULL;
}

#warning factorize me with tinit_repo_setup_svc_stopon()
static void
tinit_repo_setup_svc_starton(const struct tinit_repo * repo,
                             struct svc *              svc)
{
	assert(repo);
	assert(svc);

	const struct strarr * starton;

	starton = conf_get_starton(svc->conf);
	if (starton) {
		unsigned int nr;
		unsigned int n;

		nr = strarr_nr(starton);
		assert(nr > 0);
		for (n = 0; n < nr; n++) {
			const char * name;
			struct svc * notif;

			name = strarr_get(starton, n);
			assert(name);

			notif = tinit_repo_search_byname(repo, name);
			if (notif) {
				svc_register_starton_obsrv(notif, svc);
				continue;
			}

			tinit_warn("'%s': starton notifying service "
			           "'%s' not found.",
			           conf_get_name(svc->conf),
			           name);
		}
	}
}

static void
tinit_repo_setup_svc_stopon(const struct tinit_repo * repo,
                            struct svc *              svc)
{
	assert(repo);
	assert(svc);

	const struct strarr * stopon;

	stopon = conf_get_stopon(svc->conf);
	if (stopon) {
		unsigned int nr;
		unsigned int n;

		nr = strarr_nr(stopon);
		assert(nr > 0);
		for (n = 0; n < nr; n++) {
			const char * name;
			struct svc * notif;

			name = strarr_get(stopon, n);
			assert(name);

			notif = tinit_repo_search_byname(repo, name);
			if (notif) {
				svc_register_stopon_obsrv(notif, svc);
				continue;
			}

			tinit_warn("'%s': stopon notifying service "
			           "'%s' not found.",
			           conf_get_name(svc->conf),
			           name);
		}
	}
}

static int
tinit_repo_read_dirent(DIR * dir, const struct dirent ** ent)
{
	const struct dirent * e;
	int                   err;

	errno = 0;
	e = readdir(dir);
	if (e) {
		*ent = e;
		return 0;
	}

	err = errno;
	assert(err != EBADF);
	if (!err)
		return -ENOENT;

	tinit_repo_err(err, "cannot retrieve service configuration entry");

	return -err;
}

static int
tinit_repo_load_svc(struct tinit_repo *   repo,
                    const struct dirent * ent,
                    char                           path[PATH_MAX])
{
	const char *      ext;
	struct conf_svc * conf;
	struct svc *      svc;

	/* Skip non regular files. */
	if (ent->d_type != DT_REG)
		return 0;

	/* Search for a '.conf' file name extension and skip if not found. */
	ext = strrchr(ent->d_name, '.');
	if (!ext || strcmp(&ext[1], "conf")) {
		tinit_debug("'" CONFIG_TINIT_INCLUDE_DIR "/%s': "
		            "skipping service configuration entry.",
		            ent->d_name);
		return 0;
	}

	/* Build absolute path and give it to service configuration parser. */
	strcpy(&path[TINIT_INCLUDE_DIR_LEN + 1], ent->d_name);
	conf = conf_create_from_file(path);
	if (!conf)
		/* Skip invalid configuration items. */
		return (errno != ENOMEM) ? 0 : -ENOMEM;

	/* Create a service descriptor using loaded configuration. */
	svc = svc_create(conf);
	if (!svc) {
		assert(errno == ENOMEM);
		conf_destroy(conf);
		return -ENOMEM;
	}

	/* Register main service repository. */
	stroll_dlist_append(&repo->list, &svc->repo);

	return 0;
}

int
tinit_repo_load(struct tinit_repo * repo)
{
	assert(repo);
	assert((TINIT_INCLUDE_DIR_LEN + 1 + NAME_MAX) <= PATH_MAX);

	int          ret;
	DIR *        dir;
	char *       path;
	struct svc * svc;

	dir = opendir(CONFIG_TINIT_INCLUDE_DIR);
	if (!dir) {
		ret = errno;
		assert(ret != EBADF);

		tinit_repo_err(ret,
		               "cannot open service configuration directory");
		return -ret;
	}

	path = malloc(PATH_MAX);
	if (!path) {
		ret = -errno;
		goto close;
	}

	memcpy(path, CONFIG_TINIT_INCLUDE_DIR, TINIT_INCLUDE_DIR_LEN);
	path[TINIT_INCLUDE_DIR_LEN] = '/';

	do {
		const struct dirent * ent;

		ret = tinit_repo_read_dirent(dir, &ent);
		if (ret) {
			if (ret == -ENOENT)
				/* End of directory iteration. */
				ret = 0;
			break;
		}

		ret = tinit_repo_load_svc(repo, ent, path);
	} while (!ret);

	if (ret) {
		tinit_repo_clear(repo);
		goto free;
	}

	tinit_repo_foreach(repo, svc) {
		tinit_repo_setup_svc_starton(repo, svc);
		tinit_repo_setup_svc_stopon(repo, svc);
	}

	tinit_debug("service configuration loaded.");

free:
	free(path);
close:
	closedir(dir);

	return ret;
}

#if defined(CONFIG_TINIT_DEBUG)

void
tinit_repo_clear(struct tinit_repo * repo)
{
	assert(repo);

	while (!stroll_dlist_empty(&repo->list)) {
		struct svc * svc;

		svc = stroll_dlist_entry(stroll_dlist_next(&repo->list),
		                         struct svc,
		                         repo);
		assert(svc);
		stroll_dlist_remove(&svc->repo);

		svc_destroy(svc);
	}
}

#endif /* defined(CONFIG_TINIT_DEBUG) */
