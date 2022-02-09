#include "mnt.h"
#include "log.h"
#include <utils/slist.h>
#include <utils/path.h>
#include <utils/pwd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <mntent.h>
#include <assert.h>
#include <errno.h>
#include <sys/mount.h>

struct mnt_table {
	struct slist points;
	struct slist types;
};

/******************************************************************************
 * (Un)mount syscall helpers.
 ******************************************************************************/

#if !defined(DOCKER)

static int
mnt_mount(const char *  dev,
          const char *  dir,
          const char *  type,
          unsigned long flags,
          const void *  opts)
{
	assert(upath_validate_path_name(dev));
	assert(upath_validate_path_name(dir));
	assert(type);
	assert(type[0]);

	if (mount(dev, dir, type, flags, opts)) {
		assert(errno != EACCES);
		assert(errno != EBUSY);
		assert(errno != EFAULT);
		assert(errno != ENAMETOOLONG);
		assert(errno != ENOTBLK);
		assert(errno != ENXIO);
		assert(errno != EPERM);

		return -errno;
	}

	return 0;
}

#endif /* defined(DOCKER) */

static int
mnt_remount(const char * dir, unsigned long flags, const char * opts)
{
	assert(upath_validate_path_name(dir));

	if (mount(NULL, dir, NULL, MS_REMOUNT | flags, opts)) {
		assert(errno != EACCES);
		assert(errno != EFAULT);
		assert(errno != ENAMETOOLONG);
		assert(errno != ENOTBLK);
		assert(errno != ENXIO);
		assert(errno != EPERM);

		return -errno;
	}

	return 0;
}

/******************************************************************************
 * Filesystem types handling.
 ******************************************************************************/

#define FSTYPE_MAX (256U)

struct mnt_fstype {
	struct slist_node node;
	bool              skip;
	char              name[FSTYPE_MAX];
	int               mask;
};

static struct mnt_fstype *
mnt_create_fstype(const char * name, bool nodev)
{
	assert(name);

	struct mnt_fstype * type;
	size_t              len;

	len = strnlen(name, sizeof(type->name));
	if (!len) {
		errno = ENODATA;
		return NULL;
	}
	if (len >= sizeof(type->name)) {
		errno = ENAMETOOLONG;
		return NULL;
	}

	type = malloc(sizeof(*type));
	if (!type)
		return NULL;

	/*
	 * Do not bother unmounting pseudo filesystems.
	 *
	 * Also, FUSE filesystems require complex userspace support to unmount
	 * (fusermount utility). These should have been handled through standard
	 * service stop commands definition.
	 * These do not support read-only remounts neither: just skip them.
	 */
	if (nodev || !strcmp(name, "fuse"))
		type->skip = true;
	else
		type->skip = false;

	memcpy(type->name, name, len);
	type->name[len] = '\0';

	/* These ones do not support forced unmounts... */
	if (strcmp(name, "nfs") &&
	    strcmp(name, "cifs") &&
	    strcmp(name, "9p") &&
	    strcmp(name, "ceph") &&
	    strcmp(name, "lustre"))
		type->mask = ~(0U);
	else
		type->mask = ~MNT_FORCE;

	return type;
}

static void
mnt_destroy_fstype(struct mnt_fstype * type)
{
	assert(type);

	free(type);
}

static void
mnt_release_fstypes(struct mnt_table * table)
{
	assert(table);

	while (!slist_empty(&table->types)) {
		struct mnt_fstype * type;

		type = slist_entry(slist_dqueue(&table->types),
		                   struct mnt_fstype,
		                   node);
		mnt_destroy_fstype(type);
	}
}

static int
mnt_load_fstypes(struct mnt_table * table)
{
	assert(table);

	FILE *  file;
	char *  ln;
	size_t  max = LINE_MAX;
	int     ret;

	file = fopen(CONFIG_TINIT_FSTYPE_PATH, "r");
	if (!file)
		return -errno;

	ln = malloc(max);
	if (!ln) {
		ret = -errno;
		goto close;
	}

	slist_init(&table->types);

	while (!feof(file)) {
		ssize_t             len;
		char *              left;
		const char *        fst;
		const char *        snd;
		struct mnt_fstype * type;

		len = getline(&ln, &max, file);
		if (len < 0) {
			assert(errno != EINVAL);
			
			if (errno == EAGAIN)
				/* End of file. */
				break;

			ret = -errno;
			tinit_err("cannot fetch filesytem type infos: %s (%d).",
			          strerror(-ret),
			          -ret);
			goto free;
		}

		left = ln;
		fst = strsep(&left, " \t\n");
		if (!left ||
		    (fst[0] && strcmp(fst, "nodev")))
			continue;

		snd = strsep(&left, " \t\n");
		if (!left || !snd[0])
			continue;

		type = mnt_create_fstype(snd, !!fst[0]);
		if (!type) {
			if (errno == ENOMEM) {
				ret = -ENOMEM;
				goto free;
			}

			tinit_warn("'%s': "
			           "cannot load filesytem type infos: %s (%d).",
			           snd,
			           strerror(errno),
			           errno);
			continue;
		}

		slist_nqueue(&table->types, &type->node);
	}

	ret = 0;

free:
	free(ln);
close:
	fclose(file);

	if (ret)
		mnt_release_fstypes(table);

	return ret;
}

static const struct mnt_fstype *
mnt_find_fstype(struct mnt_table * table, const char * fstype)
{
	struct mnt_fstype * type;

	slist_foreach_entry(&table->types, type, node)
		if (!strncmp(type->name, fstype, sizeof(type->name)))
			return type;

	return NULL;
}

/******************************************************************************
 * Initial mount handling.
 ******************************************************************************/

#define TINIT_PSEUDO_MNT_BASE_FLAGS \
	(MS_NODIRATIME | MS_NOEXEC | MS_NOSUID)

#if !defined(DOCKER)

static int
mount_pseudo(const char *  dir,
             const char *  type,
             unsigned long flags,
             const void *  opts)
{
	int err;

	err = mnt_mount(type, dir, type, flags, opts);
	if (err) {
		tinit_err("'%s': cannot mount filesystem: %s (%d).",
		          dir,
		          strerror(-err),
		          -err);
		return err;
	}

	return 0;
}

#define TINIT_MQUEUE_MNTPT "/dev/mqueue"
#define TINIT_MQUEUE_MODE  UCONCAT(0, CONFIG_TINIT_MQUEUE_MODE)

static int
mount_mqueue(void)
{
	int   err;
	gid_t gid = 0;

	if (upath_mkdir(TINIT_MQUEUE_MNTPT, S_IRWXU)) {
		assert(errno != EFAULT);
		assert(errno != ENAMETOOLONG);
		assert(errno != EPERM);

		tinit_err("'" TINIT_MQUEUE_MNTPT "': "
		          "cannot create message queue mount point: %s (%d).",
		          strerror(errno),
		          errno);

		return -errno;
	}

	err = mount_pseudo(TINIT_MQUEUE_MNTPT,
	                   "mqueue",
	                   TINIT_PSEUDO_MNT_BASE_FLAGS | MS_NOATIME | MS_NODEV,
	                   NULL);
	if (err)
		return err;

	err = upath_chmod(TINIT_MQUEUE_MNTPT, TINIT_MQUEUE_MODE);
	if (err) {
		tinit_warn("cannot set message queue mount point permissions: "
		           "%s (%d)",
		           strerror(-err),
		           -err);
		return 0;
	}

	err = upwd_get_gid_byname(CONFIG_TINIT_MQUEUE_GROUP, &gid);
	if (err) {
		tinit_warn("invalid '" CONFIG_TINIT_MQUEUE_GROUP "' "
		           "message queue group: %s (%d).",
		           strerror(-err),
		           -err);
		return 0;
	}

	err = upath_chown(TINIT_MQUEUE_MNTPT, 0, gid);
	if (err)
		tinit_warn("cannot set message queue mount point ownership: "
		           "%s (%d)",
		           strerror(-err),
		           -err);

	return 0;
}

#define TINIT_DEV_MNTPT "/dev"

static int
mount_devfs(void)
{
	int err;

	err = mount_pseudo(TINIT_DEV_MNTPT,
	                   "devtmpfs",
	                   TINIT_PSEUDO_MNT_BASE_FLAGS | MS_NOATIME,
	                   NULL);
	if (err)
		return err;

	/*
	 * For some reason, additional mount options are only applied at
	 * remounting time...
	 * Mode option will be ignored and it is required to perform a chmod(2)
	 * to modify permissions of mount point directory "/dev".
	 */
	err = mnt_remount(TINIT_DEV_MNTPT,
	                  TINIT_PSEUDO_MNT_BASE_FLAGS | MS_NOATIME,
	                  CONFIG_TINIT_DEV_MNT_OPTS);
	if (err)
		tinit_warn("cannot set device filesystem mount options: "
		           "%s (%d).",
		          strerror(-err),
		          -err);

	/* Setup sane secure defaults for devices below. */
	err = upath_chmod(TINIT_DEV_MNTPT "/kmsg", S_IRUSR | S_IWUSR);
	err = upath_chmod(TINIT_DEV_MNTPT "/ptmx", S_IRUSR | S_IWUSR);
	err = upath_chmod(TINIT_DEV_MNTPT "/random", S_IRUSR | S_IWUSR);
	err = upath_chmod(TINIT_DEV_MNTPT "/urandom", S_IRUSR | S_IWUSR |
	                                              S_IRGRP |
	                                              S_IROTH);

	return 0;
}

static int
remount_root(void)
{
	int err;

	err = mnt_remount("/",
	                  MS_RDONLY | MS_NODIRATIME | MS_NOATIME |
	                  MS_NOSUID | MS_NODEV,
	                  CONFIG_TINIT_ROOT_MNT_OPTS);
	if (err) {
		tinit_err("cannot remount root filesystem: %s (%d).",
		          strerror(-err),
		          -err);
		return err;
	}

	return 0;
}

int
mnt_mount_all(void)
{
	int err;

	err = mount_pseudo("/proc",
	                   "proc",
	                   TINIT_PSEUDO_MNT_BASE_FLAGS | MS_NOATIME | MS_NODEV,
	                   CONFIG_TINIT_PROC_MNT_OPTS);
	if (err)
		return err;

	err = mount_pseudo("/sys",
	                   "sysfs",
	                   TINIT_PSEUDO_MNT_BASE_FLAGS | MS_NOATIME | MS_NODEV,
	                   NULL);
	if (err)
		return err;

	err = mount_devfs();
	if (err)
		return err;

	err = mount_mqueue();
	if (err)
		return err;

	err = mount_pseudo("/run",
	                   "tmpfs",
	                   TINIT_PSEUDO_MNT_BASE_FLAGS | MS_RELATIME,
	                   CONFIG_TINIT_RUN_MNT_OPTS);
	if (err)
		return err;

	err = remount_root();
	if (err)
		return err;

	tinit_debug("initial filesystems mounted.");

	return 0;
}

#else  /* defined(DOCKER) */

int
mnt_mount_all(void)
{
	tinit_debug("initial filesystems mounted.");

	return 0;
}

#endif /* !defined(DOCKER) */

/******************************************************************************
 * Unmount handling.
 ******************************************************************************/

struct mnt_point {
	struct slist_node node;
	char              fsname[PATH_MAX];
	char              dir[PATH_MAX];
	char              type[FSTYPE_MAX];
};

static int
mnt_strcpy(char * dest, const char * src, size_t size)
{
	assert(dest);
	assert(src);
	assert(size);

	size_t len;

	len = strnlen(src, size);
	if (len >= size)
		return -ENAMETOOLONG;

	memcpy(dest, src, len);
	dest[len] = '\0';

	return 0;
}

static struct mnt_point *
mnt_create_point(const struct mntent * entry)
{
	assert(entry);

	struct mnt_point * pt;
	int                err;

	pt = malloc(sizeof(*pt));
	if (!pt)
		return NULL;

	err = mnt_strcpy(pt->fsname, entry->mnt_fsname, sizeof(pt->fsname));
	if (err)
		goto free;

	err = mnt_strcpy(pt->dir, entry->mnt_dir, sizeof(pt->dir));
	if (err)
		goto free;

	err = mnt_strcpy(pt->type, entry->mnt_type, sizeof(pt->type));
	if (err)
		goto free;

	return pt;

free:
	free(pt);

	if (err != -ENOMEM)
		tinit_err("'%.16s': cannot probe mountpoint: %s (%d).",
		          entry->mnt_dir,
		          strerror(-err),
		          -err);

	errno = -err;
	return NULL;
}

static void
mnt_destroy_point(struct mnt_point * point)
{
	assert(point);

	free(point);
}

static void
mnt_release_points(struct mnt_table * table)
{
	assert(table);

	while (!slist_empty(&table->points)) {
		struct mnt_point * pt;

		pt = slist_entry(slist_dqueue(&table->points),
		                 struct mnt_point,
		                 node);
		mnt_destroy_point(pt);
	}
}

static int
mnt_load_points(struct mnt_table * table)
{
	assert(table);

	FILE * mnt;
	int    ret;

	mnt = setmntent(CONFIG_TINIT_MNTTAB_PATH, "r");
	if (!mnt) {
		assert(errno != EINVAL);
		return -errno;
	}

	slist_init(&table->points);

	while (true) {
		struct mntent    * ent;
		struct mnt_point * pt;

		ent = getmntent(mnt);
		if (!ent) {
			if (errno = EAGAIN)
				/* End of file. */
				break;

			ret = -errno;
			tinit_err("cannot fetch mount point infos: %s (%d).",
			          strerror(-ret),
			          -ret);
			goto end;
		}

		pt = mnt_create_point(ent);
		if (!pt) {
			if (errno == ENOMEM) {
				ret = -ENOMEM;
				goto end;
			}

			tinit_err("'%s': "
			          "cannot load mount point infos: %s (%d).",
			          ent->mnt_fsname,
			          strerror(errno),
			          errno);
			continue;
		}

		slist_append(&table->points,
		             slist_head(&table->points),
		             &pt->node);
	}

	ret = 0;

end:
	endmntent(mnt);

	if (ret)
		mnt_release_points(table);

	return ret;
}

static int
mnt_open_table(struct mnt_table * table)
{
	int err;

	err = mnt_load_points(table);
	if (err)
		return err;

	/*
	 * Do not bother if loading filesystem types has failed. We still want
	 * ot unmount as much mount points as possible.
	 */
	mnt_load_fstypes(table);

	return 0;
}

static void
mnt_close_table(struct mnt_table * table)
{
	mnt_release_fstypes(table);
	mnt_release_points(table);
}

void
mnt_umount_all(int flags)
{
	assert(!(flags & ~(MNT_FORCE | MNT_DETACH)));

	struct mnt_table   tab;
	struct mnt_point * pt;
	int                ret = 0;

	if (mnt_open_table(&tab))
	    goto err;

	slist_foreach_entry(&tab.points, pt, node) {
		const struct mnt_fstype * type;

		if (!strcmp(pt->dir, "/"))
			/*
			 * Skip root FS since required to be remounted
			 * read-only.
			 */
			continue;

		type = mnt_find_fstype(&tab, pt->type);
		assert(type);
		if (type->skip) {
			tinit_debug("'%s': skipping '%s' filesystem...",
			            pt->dir,
			            type->name);
			continue;
		}

		tinit_debug("'%s': unmounting '%s' filesystem...",
		            pt->dir,
		            pt->type);

		if (umount2(pt->dir, (flags & type->mask) | UMOUNT_NOFOLLOW)) {
			int err = -errno;

			assert(err != -EAGAIN);
			assert(err != -EFAULT);
			assert(err != -EINVAL);
			assert(err != -ENAMETOOLONG);
			assert(err != -ENOENT);
			assert(err != -EPERM);

			if (err == -EBUSY) {
				err = mnt_remount(pt->dir, MS_RDONLY, NULL);
				if (err)
					tinit_err("'%s': "
					          "cannot remount read-only: "
					          "%s (%d).",
					          pt->dir,
					          strerror(-err),
					          -err);
				else
					tinit_warn("'%s': remounted read-only.",
					           pt->dir);
			}
			else
				tinit_err("'%s': cannot unmount: %s (%d).",
				          pt->dir,
				          strerror(-err),
				          -err);

			if (err && !ret)
				ret = err;
		}
	}

	mnt_close_table(&tab);

	remount_root();

	if (ret)
		goto err;

	tinit_info("unmounted all filesystems.");

	return;

err:
	tinit_warn("failed to unmount all filesystems.");
}
