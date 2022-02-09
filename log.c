#include "log.h"
#include <utils/pwd.h>
#include <fcntl.h>
#include <assert.h>

#define CONFIG_TINIT_STDLOG_FORMAT   (ELOG_SEVERITY_FMT)
#define CONFIG_TINIT_STDLOG_SEVERITY (ELOG_WARNING_SEVERITY)

#define CONFIG_TINIT_MQLOG_SEVERITY  ELOG_NOTICE_SEVERITY
#define CONFIG_TINIT_MQLOG_FACILITY  LOG_LOCAL0
#define CONFIG_TINIT_MQLOG_NAME      "/init"
#define CONFIG_TINIT_MQLOG_MODE      (S_IWUSR | S_IRGRP)
#define CONFIG_TINIT_MQLOG_DEPTH     (64U)
#define CONFIG_TINIT_MQLOG_GROUP     "elogd"

static struct elog_multi tinit_toplog;
static struct elog_stdio tinit_stdlog;
static struct elog *     tinit_mqlog;

static struct elog_stdio_conf tinit_stdlog_conf = {
	.super.severity = CONFIG_TINIT_STDLOG_SEVERITY,
	.format         = CONFIG_TINIT_STDLOG_FORMAT
};

void
tinit_parse_stdlog_arg(char * __restrict arg, size_t len __unused)
{
	assert(arg);
	assert((size_t)ustr_parse(arg, TINIT_ARG_MAX) == len);

	const struct elog_stdio_conf dflt = tinit_stdlog_conf;
	struct elog_parse            parse;
	int                          err;

	elog_init_stdio_parse(&parse, &tinit_stdlog_conf, &dflt);

	err = elog_parse_stdio_severity(&parse, &tinit_stdlog_conf, arg);
	if (err) {
		tinit_warn("invalid standard logger argument: %s.",
		           parse.error);
		goto fini;
	}

	err = elog_realize_parse(&parse,
	                         (struct elog_conf *)&tinit_stdlog_conf);
	assert(!err);

	elog_reconf_stdio(&tinit_stdlog, &tinit_stdlog_conf);

fini:
	elog_fini_parse(&parse);
}

void
tinit_preinit_logs(void)
{
	elog_init_stdio(&tinit_stdlog, &tinit_stdlog_conf);
	tinit_logger = (struct elog *)&tinit_stdlog;

	elog_init_multi(&tinit_toplog, NULL);
}

static struct elog_mqueue_conf tinit_mqlog_conf = {
	.super.severity = CONFIG_TINIT_MQLOG_SEVERITY,
	.facility       = CONFIG_TINIT_MQLOG_FACILITY,
	.name           = CONFIG_TINIT_MQLOG_NAME
};

void
tinit_parse_mqlog_arg(char * __restrict arg, size_t len __unused)
{
	assert(arg);
	assert((size_t)ustr_parse(arg, TINIT_ARG_MAX) == len);

	const struct elog_mqueue_conf dflt = tinit_mqlog_conf;
	struct elog_parse             parse;
	int                           err;

	elog_init_mqueue_parse(&parse, &tinit_mqlog_conf, &dflt);

	err = elog_parse_mqueue_severity(&parse,
	                                 (struct elog_mqueue_conf *)
	                                 &tinit_mqlog_conf,
	                                 arg);
	if (err) {
		tinit_warn("invalid message queue logger argument: %s.",
		           parse.error);
		goto fini;
	}

	err = elog_realize_parse(&parse, (struct elog_conf *)&tinit_mqlog_conf);
	assert(!err);

fini:
	elog_fini_parse(&parse);
}

static struct elog *
tinit_create_mqueue(const struct elog_mqueue_conf * conf)
{
	assert(conf);
	assert(umq_validate_name(conf->name) > 0);
	assert(!(CONFIG_TINIT_MQLOG_MODE & ~(S_IRUSR | S_IWUSR |
	                                           S_IRGRP | S_IWGRP |
	                                           S_IROTH | S_IWOTH)));
	assert(CONFIG_TINIT_MQLOG_DEPTH > 1);
	assert(ustr_parse(CONFIG_TINIT_MQLOG_GROUP, LOGIN_NAME_MAX) > 0);


	gid_t          gid;
	int            err;
	mqd_t          mqd;
	mode_t         msk;
	struct mq_attr attr = {
		.mq_maxmsg  = CONFIG_TINIT_MQLOG_DEPTH,
		.mq_msgsize = ELOG_LINE_MAX
	};
	struct elog *  log;

	err = upwd_get_gid_byname(CONFIG_TINIT_MQLOG_GROUP, &gid);
	if (err) {
		tinit_warn("invalid logger message queue group name '%s': "
		           "%s (%d).",
		           CONFIG_TINIT_MQLOG_GROUP,
		           strerror(-err),
		           -err);
		gid = 0;
	}

	msk = umask(~CONFIG_TINIT_MQLOG_MODE);
	mqd = umq_new(conf->name,
	              O_WRONLY | O_EXCL | O_CLOEXEC | O_NOATIME | O_NONBLOCK,
	              DEFFILEMODE,
	              &attr);
	umask(msk);

	if (mqd < 0) {
		tinit_err("cannot create logger message queue: %s (%d).",
		          strerror(-mqd),
		          -mqd);
		errno = -mqd;
		return NULL;
	}

	err = ufd_fchown(mqd, 0, gid);
	if (err) {
		tinit_err("cannot setup logger message queue permissions: "
		          "%s (%d).",
		          strerror(-err),
		          -err);
		goto close;
	}

	log = elog_create_mqueue_bymqd(mqd, conf);
	if (!log) {
		err = -errno;

		tinit_warn("cannot create message queue logger: "
		           "%s (%d).",
		           strerror(-err),
		           -err);
		goto close;
	}

	return log;

close:
	umq_close(mqd);

	errno = -err;

	return NULL;
}

void
tinit_postinit_logs(void)
{
	assert(tinit_logger);

	int err;

	err = elog_register_multi_sublog(&tinit_toplog,
	                                 (struct elog *)&tinit_stdlog);
	if (err) {
		tinit_warn("cannot register standard logger: %s (%d).",
		           strerror(-err),
		           -err);
		return;
	}

	tinit_mqlog = tinit_create_mqueue(&tinit_mqlog_conf);
	if (tinit_mqlog) {
		err = elog_register_multi_sublog(&tinit_toplog, tinit_mqlog);
		if (err) {
			tinit_warn("cannot register message queue logger: "
			           "%s (%d).",
			           strerror(-err),
			           -err);
			elog_destroy(tinit_mqlog);
			return;
		}

		tinit_logger = (struct elog *)&tinit_toplog;
	}
	else
		tinit_warn("cannot initialize message queue logger: %s (%d).",
		           strerror(errno),
		           errno);
}

void
tinit_prefini_logs(void)
{
	assert(tinit_logger);

	tinit_logger = (struct elog *)&tinit_stdlog;

	if (tinit_mqlog)
		elog_destroy(tinit_mqlog);
}

void
tinit_postfini_logs(void)
{
	elog_fini_multi(&tinit_toplog);
	elog_fini_stdio(&tinit_stdlog);

#if defined(CONFIG_TINIT_DEBUG)
	tinit_logger = NULL;
#endif /* defined(CONFIG_TINIT_DEBUG) */
}
