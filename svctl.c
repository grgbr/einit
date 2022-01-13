/* Use GNU version of basename() */
#ifndef _GNU_SOURCE
#error Requires _GNU_SOURCE to be defined !
#endif /* _GNU_SOURCE */

#include <tinit/tinit.h>
#include "conf.h"
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <libsmartcols.h>

#define VIEW_HEAD_COLOR "bold"

#define err(_fmt, ...) \
	fprintf(stderr, "%s: " _fmt ".\n", argv0, ## __VA_ARGS__)

static const char * argv0;

typedef int (svc_cmd_fn)(struct tinit_sock * sock,
                         const char *        name,
                         size_t                       len);

enum view_col {
	NAME_COL,
	ADM_COL,
	RUN_COL,
};

static struct libscols_line *
view_new_row(struct libscols_table *          view,
             const struct tinit_status_iter * iter)
{
	assert(view);
	assert(iter);

	struct libscols_line * row;
	struct conf_svc *      conf;
	enum tinit_svc_state   run;
	static const char *    states[] = {
		[TINIT_SVC_STOPPED_STAT]  = "stopped",
		[TINIT_SVC_STARTING_STAT] = "starting",
		[TINIT_SVC_READY_STAT]    = "ready",
		[TINIT_SVC_STOPPING_STAT] = "stopping"
	};

	row = scols_table_new_line(view, NULL);
	if (!row)
		return NULL;

	conf = tinit_get_status_conf(iter);
	if (conf) {
		int err;

		err = scols_line_set_data(row, NAME_COL, conf_get_name(conf));
		conf_destroy(conf);
		if (err)
			return NULL;
	}
	else {
		if (scols_line_set_data(row, NAME_COL, "??"))
			return NULL;
	}

	//scols_line_set_color(row, CUTE_VIEW_SUITE_COLOR);
	if (scols_line_set_data(row,
	                        ADM_COL,
	                        tinit_get_status_adm_state(iter) ? "on" :
	                                                            "off"))
		return NULL;

	run = tinit_get_status_run_state(iter);
	//scols_line_set_color(row, CUTE_VIEW_SUITE_COLOR);
	if (scols_line_set_data(row, RUN_COL, states[run]))
		return NULL;

	return row;
}

static struct libscols_column *
view_new_col(struct libscols_table * view,
             const char *            name,
             double                  whint,
             int                     flags)
{
	assert(view);
	assert(name);
	assert(*name);

	struct libscols_column * col;

	col = scols_table_new_column(view, name, whint, flags);
	if (!col)
		return NULL;

	scols_cell_set_color(scols_column_get_header(col), VIEW_HEAD_COLOR);

	return col;
}

static void
view_show(struct libscols_table * view)
{
	scols_print_table(view);
}

static struct libscols_table *
view_create(bool colours)
{
	struct libscols_table * view;

	view = scols_new_table();
	if (!view)
		return NULL;

	scols_table_enable_colors(view, colours);

	if (!view_new_col(view, "NAME", 0.3, 0))
		goto unref;

	if (!view_new_col(view, "ADM", 0.1, 0))
		goto unref;

	if (!view_new_col(view, "RUN", 0.1, 0))
		goto unref;

	return view;

unref:
	scols_unref_table(view);

	return NULL;
}

static void
view_destroy(struct libscols_table * view)
{
	scols_unref_table(view);
}

static int
show_status(struct tinit_sock * sock, const char * svc_pattern)
{
	int                      ret;
	struct tinit_status_iter iter;
	struct libscols_table *  view;

	ret = tinit_parse_svc_pattern(svc_pattern);
	if (ret < 0) {
		err("'%s': invalid service pattern", svc_pattern);
		return ret;
	}

	ret = tinit_load_status(sock, svc_pattern, ret, &iter);
	if (ret) {
		err("cannot load service status: %s (%d)",
		    strerror(-ret),
		    -ret);
		return ret;
	}

	view = view_create(!!isatty(STDOUT_FILENO));
	if (!view) {
		err("cannot create table view");
		return -ENOMEM;
	}

	while (true) {
		if (!view_new_row(view, &iter)) {
			err("cannot create table view row");
			ret = -ENOMEM;
			break;
		}

		ret = tinit_step_status(&iter);
		if (ret) {
			if (ret != -ENOENT)
				err("cannot retrieve service status: %s (%d)",
				    strerror(-ret),
				    -ret);
			break;
		}
	}

	if (ret == -ENOENT) {
		view_show(view);
		ret = 0;
	}

	view_destroy(view);

	return ret;
}

static int
do_svc_cmd(struct tinit_sock * sock,
           const char *        svc_name,
           const char *        cmd_name,
           svc_cmd_fn *                 do_cmd)
{
	int ret;

	ret = tinit_parse_svc_name(svc_name);
	if (ret < 0) {
		err("'%s': invalid service name", svc_name);
		return ret;
	}

	ret = do_cmd(sock, svc_name, ret);
	if (ret) {
		err("'%s': cannot %s service: %s (%d)",
		    svc_name,
		    cmd_name,
		    strerror(-ret),
		    -ret);
		return ret;
	}

	return 0;
}

static void
usage(void)
{
	fprintf(stderr, "Usage: %s CMD\n", argv0);
}

int
main(int argc, char * const argv[])
{
	struct tinit_sock sock;
	int               err = -EINVAL;

	argv0 = basename(argv[0]);

	if (argc != 3) {
		err("missing arguments");
		usage();
		return EXIT_FAILURE;
	}

	srandom(time(NULL));
	if (tinit_open_sock(&sock, (uint16_t)random()))
		return EXIT_FAILURE;

	if (!strcmp(argv[1], "status"))
	    err = show_status(&sock, argv[2]);
	else if (!strcmp(argv[1], "start"))
	    err = do_svc_cmd(&sock, argv[2], "start", tinit_start_svc);
	else if (!strcmp(argv[1], "stop"))
	    err = do_svc_cmd(&sock, argv[2], "stop", tinit_stop_svc);
	else if (!strcmp(argv[1], "restart"))
	    err = do_svc_cmd(&sock, argv[2], "restart", tinit_restart_svc);
	else if (!strcmp(argv[1], "reload"))
	    err = do_svc_cmd(&sock, argv[2], "reload", tinit_reload_svc);
	else if (!strcmp(argv[1], "switch"))
	    err = do_svc_cmd(&sock, argv[2], "target", tinit_switch_target);
	else {
		err("'%s': unknown command", argv[1]);
		usage();
		goto close;
	}

close:
	tinit_close_sock(&sock);

	return err ? EXIT_FAILURE : EXIT_SUCCESS;
}
