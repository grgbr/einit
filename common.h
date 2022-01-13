#ifndef _TINIT_COMMON_H
#define _TINIT_COMMON_H

#include <tinit/tinit.h>
#include <elog/elog.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>

#define CONFIG_TINIT_SYSCONFDIR  CONFIG_SYSCONFDIR "/tinit"
#define CONFIG_TINIT_RUNSTATEDIR CONFIG_RUNSTATEDIR
#define CONFIG_TINIT_INCLUDE_DIR CONFIG_TINIT_SYSCONFDIR "/services"

#define tinit_emerg(_format, ...) \
	elog_log(tinit_logger, ELOG_EMERG_SEVERITY, _format, ##__VA_ARGS__)

#define tinit_crit(_format, ...) \
	elog_log(tinit_logger, ELOG_CRIT_SEVERITY, _format, ##__VA_ARGS__)

#define tinit_err(_format, ...) \
	elog_log(tinit_logger, ELOG_ERR_SEVERITY, _format, ##__VA_ARGS__)

#define tinit_warn(_format, ...) \
	elog_log(tinit_logger, ELOG_WARNING_SEVERITY, _format, ##__VA_ARGS__)

#define tinit_notice(_format, ...) \
	elog_log(tinit_logger, ELOG_NOTICE_SEVERITY, _format, ##__VA_ARGS__)

#define tinit_info(_format, ...) \
	elog_log(tinit_logger, ELOG_INFO_SEVERITY, _format, ##__VA_ARGS__)

#if defined(CONFIG_TINIT_DEBUG)

#define tinit_debug(_format, ...) \
	elog_log(tinit_logger, ELOG_DEBUG_SEVERITY, _format, ##__VA_ARGS__)

#else  /* !defined(CONFIG_TINIT_DEBUG) */

#define tinit_debug(_format, ...)

#endif /* defined(CONFIG_TINIT_DEBUG) */

#define TINIT_ARG_MAX      (256U)
#define TINIT_COMM_MAX     (16U)
#define TINIT_SVC_NAME_MAX (32U)

#define LOWER_CHARSET      "abcdefghijklmnopqrstuvwxyz"
#define UPPER_CHARSET      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define ALPHA_CHARSET      LOWER_CHARSET UPPER_CHARSET
#define DIGIT_CHARSET      "0123456789"
#define ALNUM_CHARSET      ALPHA_CHARSET DIGIT_CHARSET
#define GRAPH_CHARSET \
	"!\"#$%&'()*+,-./" \
	DIGIT_CHARSET \
	":;<=>?@" \
	UPPER_CHARSET \
	"[\\]^_`" \
	LOWER_CHARSET \
	"{|}~"
#define SPACE_CHARSET \
	" \f\n\r\t\v"
#define PRINT_CHARSET      GRAPH_CHARSET SPACE_CHARSET

extern int
tinit_probe_inval_char(const char * name, size_t len);

extern int
tinit_check_svc_name(const char * name, size_t len);

extern int
tinit_load_comm_bypid(pid_t pid, char comm[TINIT_COMM_MAX]);

struct stat;
struct epoll_event;

extern sigset_t sig_full_msk;
extern sigset_t sig_empty_msk;

extern int sys_fstat(int fd, struct stat * status);
extern int sys_open_stdio(const char * path, int flags);
extern int sys_dup2(int old_fd, int new_fd);

#endif /* _TINIT_COMMON_H */
