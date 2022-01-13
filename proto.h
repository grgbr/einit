#ifndef _TINIT_PROTO_H
#define _TINIT_PROTO_H

#include <tinit/tinit.h>

enum tinit_msg_type {
	TINIT_STATUS_MSG_TYPE = 0,
	TINIT_START_MSG_TYPE,
	TINIT_STOP_MSG_TYPE,
	TINIT_RESTART_MSG_TYPE,
	TINIT_RELOAD_MSG_TYPE,
	TINIT_SWITCH_MSG_TYPE,
	TINIT_MSG_TYPE_NR
};

struct tinit_request_msg {
	uint16_t seq;
	uint16_t type;
	char     pattern[0];
};

struct tinit_reply_head {
	uint16_t seq;
	uint16_t type;
	uint16_t ret;
};

struct tinit_status_reply {
	struct tinit_reply_head  head;
	struct tinit_status_data statuses[0];
};

#define TINIT_SVC_PATTERN_MAX  (256U)
#define TINIT_REQUEST_SIZE_MAX \
	(sizeof(struct tinit_request_msg) + TINIT_SVC_PATTERN_MAX)
#define TINIT_MSG_SIZE_MAX     (4096U)
#define TINIT_SOCK_PATH        CONFIG_TINIT_RUNSTATEDIR "/tinit.sock"

#endif /* _TINIT_PROTO_H */
