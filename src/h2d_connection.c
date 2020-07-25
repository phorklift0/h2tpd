#include "h2d_main.h"

static WUY_LIST(h2d_connection_defer_list);

static void h2d_connection_put_defer(struct h2d_connection *c)
{
	if (!wuy_list_node_linked(&c->list_node)) {
		wuy_list_append(&h2d_connection_defer_list, &c->list_node);
	}
}

void h2d_connection_close(struct h2d_connection *c)
{
	if (c->closed) {
		return;
	}
	c->closed = true;

	if (c->is_http2) {
		http2_connection_close(c->u.h2c);
	} else if (c->u.request != NULL) {
		h2d_request_close(c->u.request);
	}

	if (c->send_buffer != NULL) {
		loop_stream_write(c->loop_stream, c->send_buffer,
				c->send_buf_pos - c->send_buffer);
		free(c->send_buffer);
	}
	loop_stream_close(c->loop_stream);

	loop_timer_delete(c->recv_timer);
	loop_timer_delete(c->send_timer);

	h2d_connection_put_defer(c);
}

static int h2d_connection_flush(struct h2d_connection *c)
{
	if (c->closed) {
		return H2D_ERROR;
	}

	if (c->send_buffer == NULL) {
		return H2D_OK;
	}
	int buf_len = c->send_buf_pos - c->send_buffer;
	if (buf_len == 0) {
		return H2D_OK;
	}

	if (c->loop_stream == NULL) { /* fake connection of subrequest */
		// TODO wake up father request
		return H2D_OK;
	}

	int write_len = loop_stream_write(c->loop_stream, c->send_buffer, buf_len);
	if (write_len < 0) {
		return H2D_ERROR;
	}

	if (write_len < buf_len) {
		if (write_len > 0) {
			memmove(c->send_buffer, c->send_buffer + write_len,
					buf_len - write_len);
			c->send_buf_pos -= write_len;
		}

		loop_timer_set_after(c->send_timer, c->conf_listen->network.send_timeout);
		return H2D_AGAIN;
	}

	loop_timer_suspend(c->send_timer);
	c->send_buf_pos = c->send_buffer;
	return H2D_OK;
}

static void h2d_connection_defer_routine(void *data)
{
	struct h2d_connection *c;
	while (wuy_list_pop_type(&h2d_connection_defer_list, c, list_node)) {
		if (c->closed) {
			free(c);
		} else {
			h2d_connection_flush(c);

			free(c->send_buffer);
			c->send_buffer = c->send_buf_pos = NULL;
		}
	}
}

int h2d_connection_make_space(struct h2d_connection *c, int size)
{
	if (c->closed) {
		return H2D_ERROR;
	}

	int buf_size = c->conf_listen->network.send_buffer_size;
	if (c->is_http2) {
		buf_size += HTTP2_FRAME_HEADER_SIZE;
	}

	if (size > buf_size) {
		printf("   !!! fatal: too small buf_size %d\n", buf_size);
		return H2D_ERROR;
	}

	/* so flush this at h2d_connection_defer_routine() */
	h2d_connection_put_defer(c);

	/* allocate buffer */
	if (c->send_buffer == NULL) {
		c->send_buffer = malloc(buf_size);
		c->send_buf_pos = c->send_buffer;
		return c->send_buffer ? buf_size : H2D_ERROR;
	}

	/* use exist buffer */
	int available = buf_size - (c->send_buf_pos - c->send_buffer);
	if (available >= size) {
		return available;
	}

	int ret = h2d_connection_flush(c);
	if (ret != H2D_OK) {
		return ret;
	}

	return buf_size - (c->send_buf_pos - c->send_buffer);
}

static int h2d_connection_on_read(loop_stream_t *s, void *data, int len)
{
	struct h2d_connection *c = loop_stream_get_app_data(s);
	// printf("on_read %d %p\n", len, c->h2c);

	c->last_recv_ts = time(NULL);

	if (c->is_http2) {
		return h2d_http2_on_read(c, data, len);
	} else {
		return h2d_http1_on_read(c, data, len);
	}
}

static void h2d_connection_on_writable(loop_stream_t *s)
{
	struct h2d_connection *c = loop_stream_get_app_data(s);

	if (h2d_connection_flush(c) != H2D_OK) {
		return;
	}

	if (c->is_http2) {
		h2d_http2_on_writable(c);
	} else {
		h2d_http1_on_writable(c);
	}
}

static void h2d_connection_on_close(loop_stream_t *s, enum loop_stream_close_reason reason)
{
	printf(" -- stream close %s, SSL: %s\n", loop_stream_close_string(reason),
			h2d_ssl_stream_error_string(s));
	if (reason == LOOP_STREAM_READ_ERROR || reason == LOOP_STREAM_WRITE_ERROR) {
		printf("errno: %s\n", strerror(errno));
	}

	h2d_connection_close(loop_stream_get_app_data(s));
}

static int64_t h2d_connection_recv_timedout(int64_t at, void *data)
{
	struct h2d_connection *c = data;
	if (c->is_http2) {
		int timeout = h2d_http2_idle_ping(c);
		if (timeout > 0) {
			return timeout;
		}
	}

	h2d_connection_close(c);
	return 0;
}
static int64_t h2d_connection_send_timedout(int64_t at, void *c)
{
	h2d_connection_close(c);
	return 0;
}

static bool h2d_connection_on_accept(loop_tcp_listen_t *loop_listen,
		loop_stream_t *s, struct sockaddr *addr)
{
	struct h2d_conf_listen *conf_listen = loop_tcp_listen_get_app_data(loop_listen);

	struct h2d_connection *c = calloc(1, sizeof(struct h2d_connection));
	if (c == NULL) {
		return false;
	}
	c->client_addr = *addr;
	c->conf_listen = conf_listen;
	c->loop_stream = s;
	c->recv_timer = loop_timer_new(h2d_loop, h2d_connection_recv_timedout, c);
	c->send_timer = loop_timer_new(h2d_loop, h2d_connection_send_timedout, c);
	loop_stream_set_app_data(s, c);

	/* set ssl */
	if (conf_listen->ssl_ctx != NULL) {
		h2d_ssl_stream_set(s, conf_listen->ssl_ctx, true);
	}

	return true;
}

static loop_tcp_listen_ops_t h2d_connection_listen_ops = {
	.reuse_port = true,
	.on_accept = h2d_connection_on_accept,
};
static loop_stream_ops_t h2d_connection_stream_ops = {
	.on_read = h2d_connection_on_read,
	.on_close = h2d_connection_on_close,
	.on_writable = h2d_connection_on_writable,

	H2D_SSL_LOOP_STREAM_UNDERLYINGS,
};

void h2d_connection_listen(wuy_array_t *listens)
{
	loop_idle_add(h2d_loop, h2d_connection_defer_routine, NULL);

	struct h2d_conf_listen *conf_listen;
	wuy_array_iter_ppval(listens, conf_listen) {

		const char *addr;
		wuy_array_iter_ppval(&conf_listen->addresses, addr) {
			loop_tcp_listen_t *loop_listen = loop_tcp_listen(h2d_loop,
					addr, &h2d_connection_listen_ops,
					&h2d_connection_stream_ops);

			if (loop_listen == NULL) {
				perror("listen fail");
				exit(H2D_EXIT_LISTEN);
			}

			loop_tcp_listen_set_app_data(loop_listen, conf_listen);
		}
	}
}
