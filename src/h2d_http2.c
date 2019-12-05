#include "h2d_main.h"

// move other where
#define h2d_litestr_equal(a_str, a_len, b_str) h2d_lenstr_equal(a_str, a_len, b_str, sizeof(b_str)-1)
static bool h2d_lenstr_equal(const char *a_str, int a_len, const char *b_str, int b_len)
{
	return a_len == b_len && memcmp(a_str, b_str, a_len) == 0;
}

/* libhttp2 hooks */

static bool h2d_http2_hook_stream_header(http2_stream_t *h2s, const char *name_str,
		int name_len, const char *value_str, int value_len)
{
	struct h2d_request *r = http2_stream_get_app_data(h2s);

	/* create request on first header */
	if (r == NULL) {
		r = h2d_request_new(http2_connection_get_app_data(
					http2_stream_get_connection(h2s)));
		r->h2s = h2s;
		http2_stream_set_app_data(h2s, r);
	}

	/* end of headers */
	if (name_str == NULL) {
		// TODO bool end_request = name_len;
		r->state = H2D_REQUEST_STATE_PROCESS_HEADERS;
		h2d_request_run(r, 0);
		return true; //  return what ??
	}

	/* parse this one header */
	if (name_str[0] == ':') {
		if (h2d_litestr_equal(name_str, name_len, ":path")) {
			r->req.url = r->req.next;
		} else if (h2d_litestr_equal(name_str, name_len, ":authority")) {
			name_str = "Host";
			name_len = 4;
			r->req.host = r->req.next;
		} else if (h2d_litestr_equal(name_str, name_len, ":scheme")) {
			return true;
		} else if (h2d_litestr_equal(name_str, name_len, ":method")) {
			r->req.method = wuy_http_method(value_str, value_len);
			if (r->req.method < 0) {
				return false;
			}
			return true;
		} else {
			return false;
		}
	}

	r->req.next = h2d_header_add(r->req.next, name_str, name_len, value_str, value_len);
	return true;
}

static bool h2d_http2_hook_stream_body(http2_stream_t *h2s, const uint8_t *buf, int len)
{
	/* ignore request body */
	return true;
}

static void h2d_http2_hook_stream_close(http2_stream_t *h2s)
{
	struct h2d_request *r = http2_stream_get_app_data(h2s);
	if (r != NULL) {
		h2d_request_close(r);
	}
}

int h2d_http2_response_headers(struct h2d_request *r)
{
	struct h2d_connection *c = r->c;

	int estimate_size = (char *)r->resp.next - (char *)r->resp.buffer + 100; // TODO
	int ret = h2d_connection_make_space(c, estimate_size);
	if (ret != H2D_OK) {
		return ret;
	}

	uint8_t *pos_frame = c->send_buf_pos;
	uint8_t *pos_payload = pos_frame + HTTP2_FRAME_HEADER_SIZE;
	uint8_t *pos_end = c->send_buffer + H2D_CONNECTION_SENDBUF_SIZE;
	uint8_t *p = pos_payload;

	int proc_len = http2_make_status_code(p, pos_end - p, r->resp.status_code);
	p += proc_len;

	if (r->resp.content_length != H2D_CONTENT_LENGTH_INIT) {
		proc_len = http2_make_content_length(p, pos_end - p, r->resp.content_length);
		p += proc_len;
	}

	struct h2d_header *h;
	for (h = r->resp.buffer; h->name_len != 0; h = h2d_header_next(h)) {
		proc_len = http2_make_header(r->h2s, p, pos_end - p, h->str,
				h->name_len, h2d_header_value(h), h->value_len);
		p += proc_len;
	}

	http2_make_frame_headers(r->h2s, pos_frame, p - pos_payload, r->resp.content_length==0, true);

	c->send_buf_pos += p - pos_frame;

	return H2D_OK;
}

void h2d_http2_response_body_packfix(struct h2d_request *r,
		uint8_t **p_buf_pos, int *p_buf_len)
{
	*p_buf_pos += HTTP2_FRAME_HEADER_SIZE;
	*p_buf_len -= HTTP2_FRAME_HEADER_SIZE;
}
int h2d_http2_response_body_pack(struct h2d_request *r, uint8_t *payload,
		int length, bool is_body_finished)
{
	http2_make_frame_body(r->h2s, payload - HTTP2_FRAME_HEADER_SIZE,
			length, is_body_finished);
	return length + HTTP2_FRAME_HEADER_SIZE;
}

static bool h2d_http2_hook_stream_response(http2_stream_t *h2s, int window)
{
	h2d_request_run(http2_stream_get_app_data(h2s), -1);
	return true; // TODO check closed?
}

static bool h2d_http2_hook_control_frame(http2_connection_t *h2c, const uint8_t *buf, int len)
{
	struct h2d_connection *c = http2_connection_get_app_data(h2c);
	memcpy(c->send_buf_pos, buf, len);
	c->send_buf_pos += len;
	return true;
}

static void h2d_http2_hook_log(http2_connection_t *h2c, const char *fmt, ...)
{
	char buffer[1000];
	va_list ap;
	va_start(ap, fmt);
	vsprintf(buffer, fmt, ap);
	va_end(ap);

	struct timeval now;
	gettimeofday(&now, NULL);

	printf("%ld.%06ld [HTTP2] %s\n", now.tv_sec, now.tv_usec, buffer);
}

void h2d_http2_init(void)
{
	http2_library_init(h2d_http2_hook_stream_header, h2d_http2_hook_stream_body,
			h2d_http2_hook_stream_close, h2d_http2_hook_stream_response,
			h2d_http2_hook_control_frame);

	if (0) {
		http2_set_log(h2d_http2_hook_log);
	}
}


/* connection event handlers */

int h2d_http2_on_read(struct h2d_connection *c, void *data, int len)
{
	http2_connection_t *h2c = c->u.h2c;

	/* h2d_http2_hook_stream_header/_body/_close() are called inside here */
	int proc_len = http2_process_input(h2c, data, len);
	if (proc_len < 0) {
		return H2D_ERROR;
	}

	if (!h2d_connection_write_blocked(c)) {
		/* h2d_http2_hook_stream_response() is called inside here */
		http2_schedular(h2c);
	}

	return proc_len;
}

void h2d_http2_on_writable(struct h2d_connection *c)
{
	/* h2d_http2_hook_stream_response() is called inside here */
	http2_schedular(c->u.h2c);
}

/* on the connection negotiated to HTTP/2, by ALPN or Upgrade */
void h2d_http2_connection_init(struct h2d_connection *c)
{
	printf("to HTTP/2\n");

	static http2_settings_t settings = {
		.max_concurrent_streams = 100,
	};
	http2_connection_t *h2c = http2_connection_new(&settings);
	//http2_connection_t *h2c = http2_connection_new(&c->conf_listen->http2.settings);
	http2_connection_set_app_data(h2c, c);

	c->is_http2 = true;
	c->u.h2c = h2c;
}
