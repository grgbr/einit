#include "repo.h"
#include "svc.h"
#include "conf.h"
#include "sigchan.h"
#include "log.h"
#include <utils/path.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>

/*
 * Maximum size of string holding path to service configuration file (including
 * terminating NULL byte).
 * Computed as:
 * * services configuration include directory path length
 *   ((sizeof(CONFIG_TINIT_INCLUDE_DIR) - 1),
 * * plus one char for '/' separator,
 * * plus maximum service configuration file name length
 *   (TINIT_SVC_NAME_MAX - 1),
 * * plus terminating NULL byte.
 */
#define TINIT_SVC_PATH_MAX \
	((sizeof(CONFIG_TINIT_INCLUDE_DIR) - 1) + \
	 1 + \
	 (TINIT_SVC_NAME_MAX - 1) + \
	 1)

/*
 * Maximum size of string holding path to target top-level directory (including
 * terminating NULL byte).
 * Computed as:
 * * maximum system path length (PATH_MAX - 1),
 * * minus one char for '/' separator,
 * * minus maximum target name length (TINIT_SVC_NAME_MAX - 1)
 * * minus maximum service name length (TINIT_SVC_NAME_MAX - 1)
 * * plus terminating NULL byte.
 */
#define TINIT_TARGET_PATH_MAX \
	((PATH_MAX - 1) - \
	 1 - \
	 (TINIT_SVC_NAME_MAX - 1) \
	 - 1 - \
	 (TINIT_SVC_NAME_MAX - 1) + \
	 1)

struct tinit_target_folder {
	DIR *  dir;
	size_t dlen;
	char * dpath;
	char * spath;
};

static const char *
tinit_target_probe_folder_svc_base(const struct tinit_target_folder * folder,
                                   const char *                       base)
{
	assert(folder);
	assert(folder->dir);
	assert(folder->dlen);
	assert(folder->dlen < TINIT_TARGET_PATH_MAX);
	assert(folder->dpath);
	assert(folder->dpath[0] == '/');
	assert(folder->spath);
	assert(base);
	assert(base[0]);

	ssize_t      len;
	const char * real;

	len = tinit_parse_svc_name(base);
	if (len < 0) {
		errno = -len;
		return NULL;
	}

	memcpy(&folder->dpath[folder->dlen], base, len + 1);
	if (!realpath(folder->dpath, folder->spath)) {
		errno = ENOENT;
		return NULL;
	}

	len = upath_validate_path(folder->spath, TINIT_SVC_PATH_MAX);
	if (len < 0) {
		errno = -len;
		return NULL;
	}
	if (((size_t)len <= sizeof(CONFIG_TINIT_INCLUDE_DIR)) ||
	    (memcmp(folder->spath,
	            CONFIG_TINIT_INCLUDE_DIR "/",
	            sizeof(CONFIG_TINIT_INCLUDE_DIR)))) {
		errno = EPERM;
		return NULL;
	}

	real = &folder->spath[sizeof(CONFIG_TINIT_INCLUDE_DIR)];
	if (tinit_probe_inval_char(real,
	                           len - sizeof(CONFIG_TINIT_INCLUDE_DIR))) {
		errno = EINVAL;
		return NULL;
	}

	return real;
}

static struct svc *
tinit_target_walk_folder(const struct tinit_target_folder * folder)
{
	assert(folder);
	assert(folder->dir);
	assert(folder->dlen);
	assert(folder->dlen < TINIT_TARGET_PATH_MAX);
	assert(folder->dpath);
	assert(folder->dpath[0] == '/');
	assert(folder->spath);

	const struct tinit_repo * repo;

	repo = tinit_repo_get();

	while (true) {
		const struct dirent * ent;
		const char *          base;
		struct svc *          svc;

		errno = 0;
		ent = readdir(folder->dir);
		if (!ent) {
			int ret = errno;

			assert(ret != EBADF);
			if (!ret)
				/* End of directory iteration. */
				return NULL;

			tinit_err("%.*s: cannot load target service entry: "
			          "%s (%d).",
			          folder->dlen,
			          folder->dpath,
			          strerror(ret),
			          ret);

			errno = ret;
			return NULL;
		}

		if (ent->d_type != DT_LNK)
			continue;

		base = tinit_target_probe_folder_svc_base(folder, ent->d_name);
		if (!base) {
			tinit_warn("%.*s/%s: invalid target service link: "
			           "%s (%d).",
			           folder->dlen,
			           folder->dpath,
			           ent->d_name,
			           strerror(errno),
			           errno);
			continue;
		}

		svc = tinit_repo_search_bypath(repo, base);
		if (svc)
			return svc;

		tinit_warn("%.*s/%s: target service not found.",
		           folder->dlen,
		           folder->dpath,
		           ent->d_name);
	}

	unreachable();
}

static int
tinit_target_init_folder(struct tinit_target_folder * folder,
                         const char *                 path,
                         const char *                 name)
{
	assert(folder);
	assert(upath_validate_path_name(path) > 0);
	assert(name);

	ssize_t plen;
	ssize_t nlen;
	char *  dpath;
	char *  spath;
	int     err;
	DIR *   dir;

	plen = upath_validate_path(path, TINIT_TARGET_PATH_MAX);
	if (plen < 0)
		return plen;

	/* Has been validated by caller. See tinit_parse_boot_target_arg(). */
	nlen = tinit_parse_svc_name(name);
	assert(nlen > 0);

	dpath = malloc(PATH_MAX);
	if (!dpath)
		return -errno;

	spath = malloc(PATH_MAX);
	if (!spath) {
		err = -errno;
		goto free_dpath;
	}

	memcpy(dpath, path, plen);
	dpath[plen] = '/';
	memcpy(&dpath[plen + 1], name, nlen + 1);

	dir = opendir(dpath);
	if (!dir) {
		err = -errno;

		assert(err != -EBADF);

		tinit_err("cannot open target directory: %s: %s (%d).",
		          dpath,
		          strerror(-err),
		          -err);
		goto free_spath;
	}

	folder->dir = dir;
	folder->dlen = plen + 1 + nlen;
	dpath[folder->dlen++] = '/';
	folder->dpath = dpath;
	folder->spath = spath;

	return 0;

free_spath:
	free(spath);
free_dpath:
	free(dpath);

	return err;
}

static void
tinit_target_fini_folder(const struct tinit_target_folder * folder)
{
	assert(folder);
	assert(folder->dir);
	assert(folder->dlen);
	assert(folder->dlen < TINIT_TARGET_PATH_MAX);
	assert(folder->dpath);
	assert(folder->dpath[0] == '/');
	assert(folder->spath);

	closedir(folder->dir);
	free((void *)folder->spath);
	free((void *)folder->dpath);
}

struct tinit_target_iter {
	unsigned int  nr;
	struct svc ** tbl;
};

static inline unsigned int
tinit_target_iter_svc_cnt(const struct tinit_target_iter * iter)
{
	assert(iter);
	assert(iter->tbl);

	return iter->nr;
}

#define tinit_target_foreach_svc(_iter, _s, _svc) \
	for (_s = 0, _svc = (_iter)->tbl[0]; \
	     _s < (_iter)->nr; \
	     _s = (_s) + 1, _svc = (_iter)->tbl[_s])

static int
tinit_target_init_iter(struct tinit_target_iter * iter,
                       const char *               dir_path,
                       const char *               name)
{
	struct tinit_target_folder folder;
	unsigned int               cnt = 0;
	unsigned int               nr = 4;
	int                        err;
	struct svc **              tbl;

	err = tinit_target_init_folder(&folder, dir_path, name);
	if (err)
		return err;

	tbl = malloc(nr * sizeof(tbl[0]));
	if (!tbl) {
		err = -errno;
		goto fini;
	}

	while (true) {
		assert(cnt <= nr);

		struct svc * svc;

		svc = tinit_target_walk_folder(&folder);
		if (!svc) {
			if (errno) {
				err = -errno;
				goto free;
			}
			/* No more folder entries. */
			break;
		}

		if (cnt == nr) {
			struct svc ** tmp;

			nr *= 2;
			tmp = reallocarray(tbl, nr, sizeof(tbl[0]));
			if (!tmp) {
				err = -errno;
				goto free;
			}

			tbl = tmp;
		}

		tbl[cnt++] = svc;
	}

	tinit_target_fini_folder(&folder);

	iter->nr = cnt;
	iter->tbl = tbl;

	return 0;

free:
	free(tbl);
fini:
	tinit_target_fini_folder(&folder);

	return err;
}

static void
tinit_target_fini_iter(const struct tinit_target_iter * iter)
{
	assert(iter);
	assert(iter->tbl);

	free((void *)iter->tbl);
}

int
tinit_target_start(const char *           dir_path,
                   const char *           name,
                   struct tinit_sigchan * chan,
                   const struct upoll *   poller)
{
	struct tinit_target_iter iter;
	int                      ret;
	unsigned int             s;
	struct svc *             svc;

	ret = tinit_target_init_iter(&iter, dir_path, name);
	if (ret)
		return ret;

	if (!tinit_target_iter_svc_cnt(&iter)) {
		tinit_err("%s/%s: no target services found.", dir_path, name);

		ret = -ENOENT;
		goto fini;
	}

	ret = tinit_sigchan_start(chan, poller);
	if (ret)
		goto fini;

	tinit_target_foreach_svc(&iter, s, svc)
		svc_start(svc);

	tinit_debug("%s/%s: target started.", dir_path, name);

fini:
	tinit_target_fini_iter(&iter);

	return ret;
}

void
tinit_target_stop(struct tinit_sigchan * chan)
{
	struct tinit_repo * repo;
	struct svc *        svc;
	unsigned int        cnt = 0;

	repo = tinit_repo_get();
	tinit_repo_foreach(repo, svc) {
		if (svc->state == TINIT_SVC_STOPPED_STAT)
			continue;

		if ((svc->state == TINIT_SVC_STARTING_STAT) ||
		    (svc->state == TINIT_SVC_READY_STAT)) {
			/* Stop active services. */
			svc_stop(svc);

			/*
			 * svc_stop() might have marked the service as
			 * stopped.
			 */
			if (svc->state == TINIT_SVC_STOPPED_STAT)
				continue;
		}

		cnt++;
	}

	tinit_sigchan_stop(chan, cnt);
}

int
tinit_target_switch(const char * dir_path, const char * name)
{
	struct tinit_target_iter  iter;
	const struct tinit_repo * repo;
	struct svc *              svc;
	int                       ret;

	ret = tinit_target_init_iter(&iter, dir_path, name);
	if (ret)
		return ret;

	if (!tinit_target_iter_svc_cnt(&iter)) {
		tinit_err("%s/%s: no target services found.", dir_path, name);

		ret = -ENOENT;
		goto fini;
	}

	repo = tinit_repo_get();
	tinit_repo_foreach(repo, svc) {
		unsigned int       c;
		const struct svc * curr;
		bool               found = false;

		tinit_target_foreach_svc(&iter, c, curr) {
			if (!strcmp(conf_get_name(curr->conf),
			            conf_get_name(svc->conf))) {
				found = true;
				break;
			}
		}

		if (!found) {
			if ((svc->state == TINIT_SVC_STARTING_STAT) ||
			    (svc->state == TINIT_SVC_READY_STAT))
				svc_stop(svc);
		}
		else {
			if ((svc->state == TINIT_SVC_STOPPED_STAT) ||
			    (svc->state == TINIT_SVC_STOPPING_STAT))
				svc_start(svc);
		}
	}

	tinit_debug("%s/%s: target started.", dir_path, name);

fini:
	tinit_target_fini_iter(&iter);

	return ret;
}
