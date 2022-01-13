#include "common.h"
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>

struct elog * tinit_logger = NULL;

int
tinit_probe_inval_char(const char * name, size_t len)
{
	assert(name);
	assert(len);
	assert(len < TINIT_SVC_NAME_MAX);
	assert(strnlen(name, TINIT_SVC_NAME_MAX) == len);

	unsigned int chr;

	if (!isalnum(name[0]))
		chr = 0;
	else if (!isalnum(name[len - 1]))
		chr = len - 1;
	else
		chr = strspn(name, ALNUM_CHARSET "-_.@");

	if (chr != len)
		return name[chr];

	return 0;
}

int
tinit_check_svc_name(const char * name, size_t len)
{
	assert(name);

	if (!len)
		return -ENODATA;
	if (len >= TINIT_SVC_NAME_MAX)
		return -ENAMETOOLONG;
	if (tinit_probe_inval_char(name, len))
		return -EINVAL;

	return 0;
}

int
tinit_load_comm_bypid(pid_t pid, char comm[TINIT_COMM_MAX])
{
	assert(pid > 0);
	assert(pid <= INT_MAX);
	assert(comm);

	char    path[sizeof("/proc/") - 1 + 10 + sizeof("/comm") - 1 + 1];
	int     fd;
	ssize_t sz;
	int     ret;

	sprintf(path, "/proc/%d/comm", (int)pid);
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
	if (fd < 0)
		return -errno;

	sz = read(fd, comm, TINIT_COMM_MAX);
	if (sz < 0) {
		ret = -errno;
		goto close;
	}
	else if ((sz <= 1) || ((size_t)sz > TINIT_COMM_MAX)) {
		ret = -EBADMSG;
		goto close;
	}

	/* Trim trailing newline / add terminating NULL byte. */
	comm[sz - 1] = '\0';

	ret = 0;

close:
	close(fd);

	return ret;
}
