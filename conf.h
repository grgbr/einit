#ifndef _TINIT_CONF_H
#define _TINIT_CONF_H

#include "common.h"
#include "strarr.h"
#include <libconfig.h>
#include <assert.h>
#include <sys/types.h>

/******************************************************************************
 * Command sequence handling.
 ******************************************************************************/

struct conf_seq {
	unsigned int           nr;
	const struct strarr ** cmds;
};

static inline unsigned int
conf_seq_nr(const struct conf_seq * seq)
{
	assert(seq);
	assert(!seq->nr || seq->cmds);

	return seq->nr;
}

static inline const struct strarr *
conf_seq_get_cmd(const struct conf_seq * seq, unsigned int cmd)
{
	assert(seq);
	assert(cmd < conf_seq_nr(seq));
	assert(seq->cmds);
	assert(seq->cmds[cmd]);

	return seq->cmds[cmd];
}

static inline const char * const *
conf_seq_get_args(const struct conf_seq * seq, unsigned int cmd)
{
	return strarr_get_members(conf_seq_get_cmd(seq, cmd));
}

struct conf_svc {
	const char *          stdin;
	const char *          stdout;
	const struct strarr * env;
	struct conf_seq       start;
	const struct strarr * daemon;
	struct conf_seq       stop;
	int                   stop_sig;
	int                   reload_sig;
	const char *          name;
	const char *          path;
	const char *          desc;
	const struct strarr * starton;
	const struct strarr * stopon;
	config_t              lib;
};

static inline const char *
conf_get_name(const struct conf_svc * conf)
{
	assert(conf);
	assert(conf->name);

	return conf->name;
}

static inline const char *
conf_get_path(const struct conf_svc * conf)
{
	assert(conf);
	assert(conf->path);

	return conf->path;
}

static inline const struct strarr *
conf_get_starton(const struct conf_svc * conf)
{
	assert(conf);

	return conf->starton;
}

static inline const struct strarr *
conf_get_stopon(const struct conf_svc * conf)
{
	assert(conf);

	return conf->stopon;
}

static inline const char * const *
conf_get_env(const struct conf_svc * conf)
{
	assert(conf);

	return (conf->env) ? strarr_get_members(conf->env) : NULL;
}

static inline unsigned int
conf_get_start_cmd_nr(const struct conf_svc * conf)
{
	assert(conf);

	return conf_seq_nr(&conf->start);
}

static inline const char * const *
conf_get_start_cmd(const struct conf_svc * conf, unsigned int index)
{
	assert(conf);

	return conf_seq_get_args(&conf->start, index);
}

static inline const char * const *
conf_get_daemon(const struct conf_svc * conf)
{
	assert(conf);

	return (conf->daemon) ? strarr_get_members(conf->daemon) : NULL;
}

static inline const char *
conf_get_daemon_bin(const struct conf_svc * conf)
{
	const char * const *args;

	args = conf_get_daemon(conf);

	return args ? args[0] : NULL;
}

static inline unsigned int
conf_get_stop_cmd_nr(const struct conf_svc * conf)
{
	assert(conf);

	return conf_seq_nr(&conf->stop);
}

static inline const char * const *
conf_get_stop_cmd(const struct conf_svc * conf, unsigned int index)
{
	assert(conf);

	return conf_seq_get_args(&conf->stop, index);
}

static inline int
conf_get_stop_sig(const struct conf_svc * conf)
{
	assert(conf);

	return conf->stop_sig;
}

static inline int
conf_get_reload_sig(const struct conf_svc * conf)
{
	assert(conf);

	return conf->reload_sig;
}

extern struct conf_svc * conf_create_from_file(const char * path);

extern void conf_destroy(struct conf_svc * conf);

extern void conf_print(const struct conf_svc * conf);

#endif /* _TINIT_CONF_H */
