#include "common.h"
#include "log.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/epoll.h>

sigset_t sig_full_msk;
sigset_t sig_empty_msk;

#warning use ufd_fstat()
int
sys_fstat(int fd, struct stat * status)
{
	assert(fd >= 0);
	assert(status);

	if (fstat(fd, status)) {
		int err = errno;

		assert(err != EBADF);
		assert(err != EFAULT);
		/* TODO: Check what happends if underlying file has vanished. */
		assert(err != ELOOP);
		assert(err != ENAMETOOLONG);
		/* TODO: Check what happends if underlying file has vanished. */
		assert(err != ENOENT);

		tinit_err("cannot fetch file descriptor filesystem status: "
		          "'%d': %s (%d).",
		          fd,
		          strerror(err),
		          err);
		return -err;
	}

	return 0;
}

int
sys_open_stdio(const char * path, int flags)
{
	assert(path);
	assert(path[0]);
	assert(strnlen(path, PATH_MAX) < PATH_MAX);

	int fd;

	fd = open(path, flags);
	if (fd < 0) {
		int err = errno;

		assert(err != EFAULT);
		assert(err != EINVAL);
		assert(err != ENAMETOOLONG);
		assert(err != ENOSPC);
		assert(err != EOPNOTSUPP);
		assert(err != EROFS);
		assert(err != ETXTBSY);
		assert(err != EWOULDBLOCK);

		tinit_err("cannot open standard I/O terminal: '%s': %s (%d).",
		          path,
		          strerror(err),
		          err);
		return -err;
	}

	return fd;
}

int
sys_dup2(int old_fd, int new_fd)
{
	assert(old_fd >= 0);
	assert(new_fd >= 0);

	if (dup2(old_fd, new_fd) < 0) {
		int err = errno;

		assert(err != EBADF);
		assert(err != EINVAL);

		tinit_err("cannot open duplicate file descriptor: "
		          "%d -> %d: %s (%d).",
		          old_fd,
		          new_fd,
		          strerror(err),
		          err);
		return -err;
	}

	return 0;
}
