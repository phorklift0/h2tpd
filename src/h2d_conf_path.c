#include "h2d_main.h"

void h2d_conf_path_stats(struct h2d_conf_path *conf_path, wuy_json_ctx_t *json)
{
	struct h2d_conf_path_stats *stats = conf_path->stats;
	wuy_json_object_int(json, "total", atomic_load(&stats->total));
	wuy_json_object_int(json, "done", atomic_load(&stats->done));
	wuy_json_object_int(json, "req_acc_ms", atomic_load(&stats->req_acc_ms));
	wuy_json_object_int(json, "react_acc_ms", atomic_load(&stats->react_acc_ms));
	wuy_json_object_int(json, "resp_acc_ms", atomic_load(&stats->resp_acc_ms));
	wuy_json_object_int(json, "total_acc_ms", atomic_load(&stats->total_acc_ms));

	wuy_json_object_object(json, "status_codes");
	int c;
#define X(s, _) c = atomic_load(&stats->status_##s); if (c != 0) wuy_json_object_int(json, #s, c);
	WUY_HTTP_STATUS_CODE_TABLE
#undef X
	c = atomic_load(&stats->status_others);
	if (c != 0) {
		wuy_json_object_int(json, "others", c);
	}
	wuy_json_object_close(json);
}

static int h2d_conf_path_name(void *data, char *buf, int size)
{
	struct h2d_conf_path *conf_path = data;
	if (conf_path->pathnames == NULL) {
		return 0;
	}
	return snprintf(buf, size, "Path(%s)>", conf_path->pathnames[0]);
}

static void h2d_conf_path_delete(void *data)
{
}

/* make sure there is one and only one content module is enabled */
static bool h2d_conf_path_post(void *data)
{
	struct h2d_conf_path *conf_path = data;

	int i;
	for (i = 0; i < h2d_module_number; i++) {
		void *mod_conf = conf_path->module_confs[i];
		if (mod_conf == NULL) {
			continue;
		}

		struct h2d_module *m = h2d_module_content_is_enabled(i, mod_conf);
		if (m == NULL) {
			continue;
		}

		if (conf_path->content == NULL) {
			conf_path->content = m;
			continue;
		}

		/* compare meta_level, pick the smaller */
		int meta_level_new = conf_path->content_meta_levels[i];
		int meta_level_old = conf_path->content_meta_levels[conf_path->content->index];
		if (meta_level_new == meta_level_old) {
			printf("duplicate content %s %s\n", conf_path->content->name, m->name);
			return false;
		}
		if (meta_level_new < meta_level_old) {
			conf_path->content = m;
		}
	}

	if (h2d_dynamic_is_enabled(&conf_path->dynamic)) {
		h2d_dynamic_set_container(&conf_path->dynamic, &h2d_conf_path_table,
				offsetof(struct h2d_conf_path, dynamic),
				h2d_conf_path_delete);

	} else if (conf_path->content == NULL && conf_path->pathnames != NULL) {
		printf("no content set, %s\n", conf_path->pathnames[0]);
		return false;
	}

	if (conf_path->name == NULL) {
		conf_path->name = conf_path->pathnames ? conf_path->pathnames[0] : "_default";
	}

	/* access log */
	struct h2d_conf_access_log *log = &conf_path->access_log;
	if (log->max_line > log->buf_size) {
		printf("expect: max_line <= buffer_size\n");
		return false;
	}
	if (log->filename == NULL) {
		log->filename = "access.log";
	}
	log->file = h2d_log_file_open(log->filename, log->buf_size);
	if (log->file == NULL) {
		return false;
	}

	conf_path->stats = wuy_shmem_alloc(sizeof(struct h2d_conf_path_stats));

	return true;
}

static struct wuy_cflua_command h2d_conf_path_access_log_commands[] = {
	{	.type = WUY_CFLUA_TYPE_STRING,
		.is_single_array = true,
		.offset = offsetof(struct h2d_conf_path, access_log.filename),
	},
	{	.name = "sampling_rate",
		.type = WUY_CFLUA_TYPE_DOUBLE,
		.offset = offsetof(struct h2d_conf_path, access_log.sampling_rate),
		.limits.d = WUY_CFLUA_LIMITS(0, 1),
		.default_value.d = 1,
	},
	{	.name = "replace_format",
		.type = WUY_CFLUA_TYPE_BOOLEAN,
		.offset = offsetof(struct h2d_conf_path, access_log.replace_format),
	},
	{	.name = "format",
		.type = WUY_CFLUA_TYPE_FUNCTION,
		.offset = offsetof(struct h2d_conf_path, access_log.format),
	},
	{	.name = "filter",
		.type = WUY_CFLUA_TYPE_FUNCTION,
		.offset = offsetof(struct h2d_conf_path, access_log.filter),
	},
	{	.name = "buffer_size",
		.type = WUY_CFLUA_TYPE_INTEGER,
		.offset = offsetof(struct h2d_conf_path, access_log.buf_size),
		.limits.n = WUY_CFLUA_LIMITS_LOWER(4 * 1024),
		.default_value.n = 16 * 1024,
	},
	{	.name = "max_line",
		.type = WUY_CFLUA_TYPE_INTEGER,
		.offset = offsetof(struct h2d_conf_path, access_log.max_line),
		.default_value.n = 2 * 1024,
	},
	{ NULL },
};
static struct wuy_cflua_command h2d_conf_path_commands[] = {
	{	.name = "_pathnames",
		.type = WUY_CFLUA_TYPE_TABLE,
		.offset = offsetof(struct h2d_conf_path, pathnames),
		.u.table = WUY_CFLUA_ARRAY_STRING_TABLE,
	},
	{	.name = "name",
		.type = WUY_CFLUA_TYPE_STRING,
		.offset = offsetof(struct h2d_conf_path, name),
	},
	{	.name = "dynamic",
		.type = WUY_CFLUA_TYPE_TABLE,
		.offset = offsetof(struct h2d_conf_path, dynamic),
		.u.table = &h2d_dynamic_conf_table,
	},
	{	.name = "error_log",
		.type = WUY_CFLUA_TYPE_TABLE,
		.offset = offsetof(struct h2d_conf_path, error_log),
		.u.table = &h2d_log_conf_table,
	},
	{	.name = "access_log",
		.type = WUY_CFLUA_TYPE_TABLE,
		.u.table = &(struct wuy_cflua_table) { h2d_conf_path_access_log_commands },
	},
	{	.type = WUY_CFLUA_TYPE_END,
		.u.next = h2d_module_next_path_command,
	},
};

struct wuy_cflua_table h2d_conf_path_table = {
	.commands = h2d_conf_path_commands,
	.size = sizeof(struct h2d_conf_path),
	.post = h2d_conf_path_post,
	.name = h2d_conf_path_name,
};
