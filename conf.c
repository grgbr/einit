#include "conf.h"
#include <stroll/cdefs.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

#define CONF_SETTING_MAX  (16U)
#define SVC_DESC_MAX      (128U)
#define SVC_ENV_NAME_MAX  (64U)
#define SVC_ENV_VALUE_MAX (1024U)
#define SVC_ARG_MAX       (1024U)
#define STRING_MAX        (4096U)
#define SVC_PRINT_FORMAT  "%-18s %s"

/******************************************************************************
 * Logging / printing helpers.
 ******************************************************************************/

#define conf_log_err(_setting, _format, ...) \
	conf_vlog(_setting, "   ERROR", _format ".\n", ## __VA_ARGS__)

#define conf_log_warn(_setting, _format, ...) \
	conf_vlog(_setting, " WARNING", _format ".\n", ## __VA_ARGS__)

#warning switch to elog
static void __printf(3, 4)
conf_vlog(const config_setting_t * setting,
          const char *             level,
          const char *             format,
          ...)
{
	assert(setting);
	assert(level);
	assert(format);

	const char * path;
	int          line;
	const char * name;
	va_list      args;

	path = config_setting_source_file(setting);
	assert(path);
	line = config_setting_source_line(setting);
	name = config_setting_name(setting);

	va_start(args, format);
	if (name)
		fprintf(stderr,
		        "[%s] '%s', line %d: '%.*s': ",
		        level,
		        path,
		        line,
		        CONF_SETTING_MAX,
		        name);
	else
		fprintf(stderr,
		        "[%s] '%s', line %d: ",
		        level,
		        path,
		        line);
	vfprintf(stderr, format, args);
	va_end(args);
}

static void
conf_print_strarr(const char *          title,
                  const char *          delim,
                  const struct strarr * array)
{
	assert(title);

	if (array) {
		unsigned int nr;
		unsigned int s;
		const char * str;

		nr = strarr_nr(array);
		assert(nr > 0);

		str = strarr_get(array, 0);
		assert(str);
		fprintf(stderr, SVC_PRINT_FORMAT, title, str);

		for (s = 1; s < nr; s++) {
			/*
			 * Skip NULL string to handle end of commands /
			 * environment variable markers.
			 */
			str = strarr_get(array, s);
			if (str) {
				assert(str[0]);
				fprintf(stderr, "%s%s",
				        delim,
				        strarr_get(array, s));
			}
		}

		fputc('\n', stderr);
	}
}

static void
conf_print_seq(const char *            title,
               const struct conf_seq * seq)
{
	assert(title);
	assert(seq);
	assert(!seq->nr || seq->cmds);

	if (seq->nr) {
		unsigned int c;

		conf_print_strarr(title, " ", conf_seq_get_cmd(seq, 0));

		for (c = 1; c < seq->nr; c++)
			conf_print_strarr("", " ", conf_seq_get_cmd(seq, c));
	}
}

/******************************************************************************
 * Command sequence handling.
 ******************************************************************************/

static void
conf_seq_put_cmd(const struct conf_seq * seq,
                 unsigned int                     cmd,
                 const struct strarr *   args)
{
	assert(seq);
	assert(cmd < conf_seq_nr(seq));
	assert(seq->cmds);
	assert(args);

	seq->cmds[cmd] = args;
}

static int
conf_seq_setup(struct conf_seq * seq, unsigned int nr)
{
	assert(seq);
	assert(nr);

	seq->cmds = calloc(nr, sizeof(seq->cmds[0]));
	if (!seq->cmds)
		return -errno;

	seq->nr = nr;

	return 0;
}

static void
conf_seq_release(struct conf_seq * seq)
{
	assert(seq);
	assert(!seq->nr || seq->cmds);

	if (seq->nr) {
		unsigned int c;

		for (c = 0; (c < seq->nr) && seq->cmds[c]; c++)
			strarr_destroy((struct strarr *)seq->cmds[c]);

		free(seq->cmds);
	}
}

/******************************************************************************
 * Parsing / loader helpers.
 ******************************************************************************/

typedef ssize_t
        (conf_parse_string_fn)(const config_setting_t * setting,
                               const char **            string);

static int
conf_parse_int_setting(const config_setting_t * setting, int * value)
{
	assert(setting);
	assert(value);

	if (config_setting_type(setting) != CONFIG_TYPE_INT) {
		conf_log_err(setting, "integer required");
		return -EBADMSG;
	}

	*value = config_setting_get_int(setting);

	return 0;
}

static int
conf_parse_signo_setting(const config_setting_t * setting, int * signo)
{
	int val;
	int err;

	err = conf_parse_int_setting(setting, &val);
	if (err)
		return err;

	if ((val <= 0) ||
	    ((val > SIGSYS) && (val < SIGRTMIN)) ||
	    (val > SIGRTMAX)) {
		conf_log_err(setting, "invalid signal number %d", val);
		return -ERANGE;
	}

	*signo = val;

	return 0;
}

static ssize_t
conf_parse_string_setting(const config_setting_t * setting,
                          const char **            string,
                          size_t                            max_size)
{
	assert(setting);
	assert(string);
	assert(max_size <= STRING_MAX);

	const char * str;
	size_t       len;

	str = config_setting_get_string(setting);
	if (!str) {
		conf_log_err(setting, "string required");
		return -EBADMSG;
	}

	len = strnlen(str, max_size);
	if (!len) {
		conf_log_err(setting, "empty string not allowed");
		return -ENODATA;
	}

	if (len >= max_size) {
		conf_log_err(setting,
		             "string length limited to %zu characters",
		             max_size - 1);
		return -EMSGSIZE;
	}

	*string = str;

	return (ssize_t)len;
}

static ssize_t
conf_parse_name_setting(const config_setting_t * setting,
                        const char **            name)
{
	assert(setting);
	assert(name);

	const char * str;
	ssize_t      len;
	unsigned int chr;

	len = conf_parse_string_setting(setting, &str, TINIT_SVC_NAME_MAX);
	if (len < 0)
		return len;

	assert(len > 0);
	chr = tinit_probe_inval_char(str, len);
	if (chr) {
		conf_log_err(setting, "'%c' character not allowed", chr);
		return -EINVAL;
	}

	*name = str;

	return len;
}

static ssize_t
conf_parse_cmd_arg(const config_setting_t * setting,
                   const char **            arg)
{
	assert(setting);
	assert(arg);

	const char * str;
	ssize_t      len;
	unsigned int chr;

	len = conf_parse_string_setting(setting, &str, SVC_ARG_MAX);
	if (len < 0)
		return len;

	assert(len > 0);
	chr = strspn(str, PRINT_CHARSET);
	if (chr != (size_t)len) {
		conf_log_err(setting,
		             "argument %d: '%c' character not allowed",
		             config_setting_index(setting) + 1,
		             str[chr]);
		return -EINVAL;
	}

	*arg = str;

	return len;
}

static int
conf_load_strarr_setting(const config_setting_t * setting,
                         const struct strarr **   array,
                         conf_parse_string_fn *            parse,
                         bool                              marker)
{
	assert(setting);
	assert(array);

	int             nr;
	struct strarr * arr;
	int             e;
	int             err;

	if (!config_setting_is_array(setting)) {
		conf_log_err(setting, "array required");
		return -EBADMSG;
	}

	nr = config_setting_length(setting);
	assert(nr >= 0);
	if (!nr) {
		conf_log_err(setting, "empty array not allowed");
		return -ENODATA;
	}

	/*
	 * If requested, allocate one more slot than the number of defined
	 * arguments to store the NULL "end of element list marker" as required
	 * by execve().
	 */
	arr = strarr_create(nr + (marker ? 1 : 0));
	if (!arr)
		return -ENOMEM;

	for (e = 0; e < nr; e++) {
		const config_setting_t * elm;
		const char *             str;
		ssize_t                  len;

		elm = config_setting_get_elem(setting, e);
		assert(elm);

		len = (*parse)(elm, &str);
		if (len < 0) {
			conf_log_err(setting, "parsing failed");

			err = len;
			goto destroy;
		}

		err = strarr_rep(arr, e, str, len);
		if (err)
			/* Error is always -ENOMEM: no need to log anything. */
			goto destroy;
	}

	/*
	 * Finally, we may have to set the NULL "end of element list marker" if
	 * required.
	 *
	 * Note! This is not strictly necessary since strarr_create() zero
	 * initializes the whole internal area allocated for string pointers.
	 *
	 * Leave it as a comment for sake of documentation:
	 *     if (marker) strarr_put(arr, nr, NULL);
	 */

	*array = arr;

	return 0;

destroy:
	strarr_destroy(arr);

	return err;
}

static int
conf_load_seq_setting(const config_setting_t * setting,
                      struct conf_seq *        seq)
{
	assert(setting);
	assert(seq);

	int nr;
	int c;
	int err;

	if (!config_setting_is_list(setting)) {
		conf_log_err(setting, "list required");
		return -EBADMSG;
	}

	nr = config_setting_length(setting);
	assert(nr >= 0);
	if (!nr) {
		conf_log_err(setting, "empty list not allowed");
		return  -ENODATA;
	}

	if (conf_seq_setup(seq, nr))
		return -ENOMEM;

	for (c = 0; c < nr; c++) {
		const config_setting_t * cmd;
		const struct strarr *    args;

		cmd = config_setting_get_elem(setting, c);
		assert(cmd);

		err = conf_load_strarr_setting(cmd,
		                               &args,
		                               conf_parse_cmd_arg,
		                               true);
		if (err) {
			conf_log_err(setting,
			             "command %d: parsing failed",
			             c + 1);
			goto release;
		}

		conf_seq_put_cmd(seq, c, args);
	}

	return 0;

release:
	conf_seq_release(seq);

	return err;
}

/******************************************************************************
 * Top-level configuration loaders.
 ******************************************************************************/

static int
conf_load_name(struct conf_svc *        conf,
               const config_setting_t * setting)
{
	assert(conf);
	assert(setting);

	const char *      str;
	ssize_t           len;

	len = conf_parse_name_setting(setting, &str);
	if (len < 0)
		return len;

	conf->name = strrep(str, len);
	if (!conf->name)
		return -errno;

	return 0;
}

static int
conf_load_desc(struct conf_svc *        conf,
               const config_setting_t * setting)
{
	assert(conf);
	assert(setting);

	const char * str;
	ssize_t      len;
	unsigned int chr;

	len = conf_parse_string_setting(setting, &str, SVC_DESC_MAX);
	if (len < 0)
		return len;

	assert(len > 0);
	chr = strspn(str,  GRAPH_CHARSET " ");
	if (chr != (unsigned int)len) {
		conf_log_err(setting, "'%c' character not allowed", str[chr]);
		return -EINVAL;
	}

	conf->desc = strrep(str, len);
	if (!conf->desc)
		return -errno;

	return 0;
}

static ssize_t
conf_load_stdio(const config_setting_t * setting,
                const char **            path)
{
	assert(setting);

	const char * str;
	ssize_t      len;
	unsigned int chr;

	len = conf_parse_string_setting(setting, &str, PATH_MAX);
	if (len < 0)
		return -len;

	assert(len > 0);
	chr = strspn(str, ALNUM_CHARSET "/._-");
	if (chr != (unsigned int)len) {
		conf_log_err(setting, "'%c' character not allowed", str[chr]);
		return -EINVAL;
	}

	*path = str;

	return len;
}

static int
conf_load_stdin(struct conf_svc *        conf,
                const config_setting_t * setting)
{
	const char * path;
	ssize_t      len;

	len = conf_load_stdio(setting, &path);
	if (len < 0)
		return len;

	if (((size_t)len <= (sizeof("/dev/") - 1)) ||
	    memcmp(path, "/dev/", sizeof("/dev/") - 1)) {
		conf_log_err(setting,
		             "'%s': pathname not located under /dev",
		             path);
		return -ENOTTY;
	}

	conf->stdin = strrep(path, len);
	if (!conf->stdin)
		return -errno;

	return 0;
}

static int
conf_load_stdout(struct conf_svc *        conf,
                 const config_setting_t * setting)
{
	const char * path;
	ssize_t      len;

	len = conf_load_stdio(setting, &path);
	if (len < 0)
		return len;

	conf->stdout = strrep(path, len);
	if (!conf->stdout)
		return -errno;

	return 0;
}

/*
 * Check environment variable name validity:
 * - empty name rejected,
 * - name length < SVC_ENV_NAME_MAX rejected,
 * - character set compliant with POSIX standard.
 *
 * As to environment names, POSIX standard (Open Group Base Specifications IEEE
 * Std 1003.1-2017), states that they:
 *   "consist solely of uppercase letters, digits, and the <underscore> ( '_' )
 *    from the characters defined in Portable Character Set and do not begin
 *    with a digit."
 *
 * See https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap08.html
 */
static ssize_t
conf_parse_env_var(const config_setting_t * setting,
                   const char **            var)
{
	assert(setting);
	assert(var);

	const char * str;
	size_t       len;
	unsigned int chr;

	/*
	 * As setting's parent is a group, there is no need to check for
	 * emptiness since this should have already been detected earlier as
	 * a syntax error.
	 */
	str = config_setting_name(setting);
	assert(str);
	assert(str[0]);

	len = strnlen(str, SVC_ENV_NAME_MAX);
	assert(len > 0);
	if (len >= SVC_ENV_NAME_MAX) {
		conf_log_err(setting,
		             "variable name length limited to %u characters",
		             SVC_ENV_NAME_MAX);
		return -EMSGSIZE;
	}

	if (isdigit(str[0]))
		chr = 0;
	else
		chr = strspn(str, ALNUM_CHARSET "_");

	if (chr != len) {
		conf_log_err(setting,
		             "'%c' character not allowed in variable name",
		             str[chr]);
		return -EINVAL;
	}

	*var = str;

	return len;
}

/*
 * Check environment variable value validity:
 * - empty name rejected,
 * - name length < SVC_ENV_VALUE_MAX rejected,
 * -ed to the character set described below.
 *
 * Allowed character set basically includes printable characters without:
 * - most of characters suitalbe for shell expansion,
 * - form-feed, newline, carriage return and vertical tabulation.
 */
static ssize_t
conf_parse_env_val(const config_setting_t * setting,
                  const char **            value)
{
	assert(setting);
	assert(value);

	const char * str;
	ssize_t      len;
	unsigned int chr;

	len = conf_parse_string_setting(setting, &str, SVC_ENV_VALUE_MAX);
	if (len < 0)
		return len;

	assert(len > 0);
	chr = strspn(str, ALNUM_CHARSET "\t _,-./:=@\\");
	if (chr != (size_t)len) {
		conf_log_err(setting,
		             "'%c' character not allowed in variable value",
		             str[chr]);
		return -EINVAL;
	}

	*value = str;

	return len;
}

static const char *
conf_build_env_expr(const config_setting_t * setting)
{
	assert(setting);

	const char * var;
	ssize_t      var_len;
	const char * val;
	ssize_t      val_len;
	char       * expr;

	/* Check validity of environment variable name. */
	var_len = conf_parse_env_var(setting, &var);
	if (var_len < 0) {
		errno = -var_len;
		return NULL;
	}

	/* Check validity of assigned environment variable value. */
	val_len = conf_parse_env_val(setting, &val);
	if (val_len < 0) {
		errno = -val_len;
		return NULL;
	}

	expr = malloc(var_len + sizeof('=') + val_len + 1);
	if (!expr)
		return NULL;

	memcpy(&expr[0], var, var_len);
	expr[var_len++] = '=';
	memcpy(&expr[var_len], val, val_len);
	var_len += val_len;
	expr[var_len] = '\0';

	return expr;
}

static int
conf_load_env(struct conf_svc *        conf,
              const config_setting_t * setting)
{
	assert(conf);
	assert(setting);

	int             nr;
	struct strarr * env;
	int             e;
	int             err;

	if (!config_setting_is_group(setting)) {
		conf_log_err(setting, "dictionary required");
		return -EBADMSG;
	}

	nr = config_setting_length(setting);
	assert(nr >= 0);
	if (!nr) {
		/* No environment variable definition found. */
		conf_log_err(setting, "empty dictionary not allowed");
		return -ENODATA;
	}

	/*
	 * Allocate one more slot than the number of defined environment
	 * variables to store the NULL "end of variable list marker" required by
	 * execve().
	 */
	env = strarr_create(nr + 1);
	if (!env)
		return -ENOMEM;

	for (e = 0; e < nr; e++) {
		const config_setting_t * var;
		const char *             expr;

		var = config_setting_get_elem(setting, e);
		assert(var);

		/*
		 * Pack each environment variable assignment for direct execve()
		 * usage.
		 */
		expr = conf_build_env_expr(var);
		if (!expr) {
			err = -errno;
			goto destroy;
		}

		strarr_put(env, e, expr);
	}

	/*
	 * Finally, set the NULL "end of variable list marker" required by
	 * execve().
	 *
	 * Note! This is not strictly necessary since strarr_create() zero
	 * initializes the whole internal area allocated for string pointers.
	 *
	 * Leave it as a comment for sake of documentation:
	 *   strarr_put(env, nr, NULL);
	 */

	conf->env = env;

	return 0;

destroy:
	strarr_destroy(env);

	return err;
}

static int
conf_load_starton(struct conf_svc *        conf,
                  const config_setting_t * setting)
{
	assert(conf);
	assert(setting);

	return conf_load_strarr_setting(setting,
	                                &conf->starton,
	                                conf_parse_name_setting,
	                                false);
}

static int
conf_load_stopon(struct conf_svc *        conf,
                 const config_setting_t * setting)
{
	assert(conf);
	assert(setting);

	return conf_load_strarr_setting(setting,
	                                &conf->stopon,
	                                conf_parse_name_setting,
	                                false);
}

static int
conf_load_start(struct conf_svc *        conf,
                const config_setting_t * setting)
{
	return conf_load_seq_setting(setting, &conf->start);
}

static int
conf_load_stop(struct conf_svc *        conf,
               const config_setting_t * setting)
{
	return conf_load_seq_setting(setting, &conf->stop);
}

static int
conf_load_sig_setting(struct conf_svc *        conf,
                      const config_setting_t * setting)
{
	const char * name;

	/*
	 * As setting's parent is a group, there is no need to check for
	 * emptiness since this should have already been detected earlier as
	 * a syntax error.
	 */
	name = config_setting_name(setting);
	assert(name);
	assert(name[0]);

	if (!strcmp(name, "stop"))
		return conf_parse_signo_setting(setting, &conf->stop_sig);

	if (!strcmp(name, "reload"))
		return conf_parse_signo_setting(setting, &conf->reload_sig);

	conf_log_err(setting, "invalid signal event");
	return -EINVAL;
}

static int
conf_load_signal(struct conf_svc *        conf,
                 const config_setting_t * setting)
{
	assert(conf);
	assert(setting);

	int nr;
	int s;
	int err;

	if (!config_setting_is_group(setting)) {
		conf_log_err(setting, "dictionary required");
		return -EBADMSG;
	}

	nr = config_setting_length(setting);
	assert(nr >= 0);
	if (!nr) {
		/* No signal event definition found. */
		conf_log_err(setting, "empty dictionary not allowed");
		return -ENODATA;
	}

	for (s = 0; s < nr; s++) {
		const config_setting_t * sig;

		sig = config_setting_get_elem(setting, s);
		assert(sig);

		err = conf_load_sig_setting(conf, sig);
		if (err)
			return err;
	}

	return 0;
}

static int
conf_load_daemon(struct conf_svc *        conf,
                 const config_setting_t * setting)
{
	assert(conf);
	assert(setting);

	return conf_load_strarr_setting(setting,
	                                &conf->daemon,
	                                conf_parse_cmd_arg,
	                                true);
}

typedef int (conf_load_setting_fn)(struct conf_svc *,
                                   const config_setting_t *);

struct conf_loader {
	const char           * name;
	conf_load_setting_fn * load;
};

static const struct conf_loader conf_loaders[] = {
	{ .name = "name",        .load = conf_load_name },
	{ .name = "description", .load = conf_load_desc },
	{ .name = "stdin",       .load = conf_load_stdin },
	{ .name = "stdout",      .load = conf_load_stdout },
	{ .name = "environ",     .load = conf_load_env },
	{ .name = "starton",     .load = conf_load_starton },
	{ .name = "start",       .load = conf_load_start },
	{ .name = "stopon",      .load = conf_load_stopon },
	{ .name = "stop",        .load = conf_load_stop },
	{ .name = "signal",      .load = conf_load_signal },
	{ .name = "daemon",      .load = conf_load_daemon }
};

static void
conf_fini(struct conf_svc * conf)
{
	assert(conf);

	if (conf->env)
		strarr_destroy((struct strarr *)conf->env);

	conf_seq_release(&conf->start);
	conf_seq_release(&conf->stop);

	if (conf->daemon)
		strarr_destroy((struct strarr *)conf->daemon);

	free((char *)conf->name);
	free((char *)conf->path);
	free((char *)conf->desc);

	if (conf->starton)
		strarr_destroy((struct strarr *)conf->starton);
	if (conf->stopon)
		strarr_destroy((struct strarr *)conf->stopon);
}

static bool
conf_strarr_has_dups(const struct strarr * array, const char * ref)
{
	assert(array);
	assert(ref);
	assert(ref[0]);

	unsigned int s;
	unsigned int nr = strarr_nr(array);
	const char * str;


	for (s = 0; s < nr; s++) {
		str = strarr_get(array, s);
		assert(str);
		assert(str[0]);

		if (!strcmp(ref, str))
			return true;
	}

	for (s = 0; s < (nr - 1); s++) {
		unsigned int c;

		ref = strarr_get(array, s);
		assert(ref);
		assert(ref[0]);

		for (c = s + 1; c < nr; c++) {
			str = strarr_get(array, c);
			assert(str);
			assert(str[0]);

			if (!strcmp(ref, str))
				return true;
		}
	}

	return false;
}

static int
conf_load_root(struct conf_svc * conf)
{
	assert(conf);

	const config_setting_t * root;
	const char *             msg;
	int                      nr;
	int                      s;
	int                      ret;

	/*
	 * Top-level setting always exists if the above parsing operation
	 * succeeded.
	 */
	root = config_root_setting(&conf->lib);
	assert(root);
	assert(config_setting_is_group(root));

	nr = config_setting_length(root);
	assert(nr >= 0);
	if (!nr) {
		msg = "empty configuration not allowed";
		ret = -ENODATA;
		goto err;
	}

	for (s = 0; s < nr; s++) {
		const config_setting_t * set;
		const char *             name;
		unsigned int             l;

		set = config_setting_get_elem(root, s);
		assert(set);

		/*
		 * Cannot be empty as parent setting is root and root setting is
		 * always a dictionnary.
		 */
		name = config_setting_name(set);
		assert(name);
		assert(name[0]);

		for (l = 0; l < stroll_array_nr(conf_loaders); l++) {
			if (!strcmp(name, conf_loaders[l].name))
				break;
		}

		if (l == stroll_array_nr(conf_loaders)) {
			conf_log_warn(set, "skipping unknown setting");
			continue;
		}

		ret = conf_loaders[l].load(conf, set);
		if (ret)
			break;
	}

	if (ret) {
		msg = "invalid configuration";
		goto fini_conf;
	}

	ret = -EPROTO;

	if (!conf->name) {
		msg = "missing name";
		goto fini_conf;
	}

	if (!conf_seq_nr(&conf->start) &&
	    !conf_seq_nr(&conf->stop) &&
	    !conf->daemon) {
		msg = "missing command(s)";
		goto fini_conf;
	}

	if (conf->starton && conf_strarr_has_dups(conf->starton, conf->name)) {
		msg = "duplicate starton service(s) found";
		goto fini_conf;
	}
	if (conf->stopon && conf_strarr_has_dups(conf->stopon, conf->name)) {
		msg = "duplicate stopon service(s) found";
		goto fini_conf;
	}

	if (!conf->stop_sig)
		conf->stop_sig = SIGTERM;
	if (!conf->reload_sig)
		conf->reload_sig = SIGTERM;

	return 0;

fini_conf:
	conf_fini(conf);
err:
	tinit_err("'%s': %s.", config_setting_source_file(root), msg);

	return ret;
}

static int
conf_load_file(struct conf_svc * conf, const char * path)
{
	assert(conf);
	assert(path);
	assert(path[0]);
	assert(strlen(path) < PATH_MAX);

	int ret;

	config_init(&conf->lib);

	/* Setup default include directory path. */
	config_set_include_dir(&conf->lib, CONFIG_TINIT_INCLUDE_DIR);

	/*
	 * Setup default parser options:
	 * - no setting value automatic type conversion,
	 * - no duplicate settings,
	 * - no required semicolon separators.
	 */
	config_set_options(&conf->lib, 0);

	if (!config_read_file(&conf->lib, path)) {
		switch (config_error_type(&conf->lib)) {
		case CONFIG_ERR_FILE_IO:
			ret = -errno;
			tinit_err("'%s': cannot load file: %s (%d).",
			          config_error_file(&conf->lib),
			          strerror(-ret),
			          -ret);
			goto destroy;

		case CONFIG_ERR_PARSE:
			ret = -EBADMSG;
			tinit_err("'%s': line %d: parsing failed: %s.",
			          config_error_file(&conf->lib),
			          config_error_line(&conf->lib),
			          config_error_text(&conf->lib));
			goto destroy;

		default:
			assert(0);
		}
	}

	ret = conf_load_root(conf);
	if (ret)
		goto destroy;

	path = basename(path);
	assert(strnlen(path, NAME_MAX) < NAME_MAX);

	conf->path = strdup(path);
	if (!conf->path)
		ret = -errno;

destroy:
	config_destroy(&conf->lib);

	return ret;
}

void
conf_print(const struct conf_svc * conf)
{
	assert(conf);
	assert(conf->name);

	fprintf(stderr, SVC_PRINT_FORMAT "\n", "Name:", conf->name);

	if (conf->desc)
		fprintf(stderr,
		        SVC_PRINT_FORMAT "\n",
		        "Description:",
		        conf->desc);

	if (conf->stdin)
		fprintf(stderr, SVC_PRINT_FORMAT "\n", "STDIN:", conf->stdin);

	if (conf->stdout)
		fprintf(stderr, SVC_PRINT_FORMAT "\n", "STDOUT:", conf->stdout);

	conf_print_strarr("Environment:", ", ", conf->env);

	conf_print_strarr("Start on (ready):", ", ", conf_get_starton(conf));

	conf_print_seq("Start:", &conf->start);

	conf_print_seq("Stop:", &conf->stop);

	conf_print_strarr("Daemon:", " ", conf->daemon);
}

struct conf_svc *
conf_create_from_file(const char * path)
{
	assert(path);
	assert(path[0]);
	assert(strlen(path) < PATH_MAX);

	struct conf_svc * conf;
	int               err;

	conf = calloc(1, sizeof(*conf));
	if (!conf)
		return NULL;

	err = conf_load_file(conf, path);
	if (err)
		goto free;

	return conf;

free:
	free(conf);
	errno = -err;

	return NULL;
}

void
conf_destroy(struct conf_svc * conf)
{
	conf_fini(conf);
	free(conf);
}
