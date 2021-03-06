#include <sys/types.h>
#include <signal.h>

#include "h2d_main.h"

struct h2d_conf_runtime *h2d_conf_runtime;

static int h2d_conf_runtime_name(void *data, char *buf, int size)
{
	return snprintf(buf, size, "Runtime>");
}

static const char *h2d_conf_runtime_worker_post(void *data)
{
	struct h2d_conf_runtime_worker *worker = data;

	if (worker->num == 0) {
		return WUY_CFLUA_OK;
	}
	if (worker->num < 0) {
		worker->num = sysconf(_SC_NPROCESSORS_ONLN);
		if (worker->num <= 0) {
			return "fail to get #CPU";
		}
	}
	worker->list = wuy_pool_alloc(wuy_cflua_pool, worker->num * sizeof(pid_t));

	return WUY_CFLUA_OK;
}

static void h2d_conf_runtime_worker_free(void *data)
{
	struct h2d_conf_runtime_worker *worker = data;

	for (int i = 0; i < worker->num; i++) {
		if (worker->list[i] != 0) {
			kill(worker->list[i], SIGQUIT);
		}
	}
}

static struct wuy_cflua_command h2d_conf_runtime_worker_commands[] = {
	{	.type = WUY_CFLUA_TYPE_INTEGER,
		.offset = offsetof(struct h2d_conf_runtime, worker.num),
	},
	{ NULL },
};
struct wuy_cflua_table h2d_conf_runtime_worker_table = {
	.commands = h2d_conf_runtime_worker_commands,
	.post = h2d_conf_runtime_worker_post,
	.free = h2d_conf_runtime_worker_free,
};

static struct wuy_cflua_command h2d_conf_runtime_commands[] = {
	{	.name = "worker",
		.type = WUY_CFLUA_TYPE_TABLE,
		.offset = offsetof(struct h2d_conf_runtime, worker),
		.u.table = &h2d_conf_runtime_worker_table,
	},
	{	.name = "log",
		.type = WUY_CFLUA_TYPE_TABLE,
		.offset = offsetof(struct h2d_conf_runtime, log),
		.u.table = &h2d_log_conf_table,
	},
	{ NULL },
};

struct wuy_cflua_table h2d_conf_runtime_table = {
	.commands = h2d_conf_runtime_commands,
	.size = sizeof(struct h2d_conf_runtime),
	.name = h2d_conf_runtime_name,
};
