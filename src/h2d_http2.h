#ifndef H2D_HTTP2_H
#define H2D_HTTP2_H

#include "h2d_connection.h"

int h2d_http2_on_read(struct h2d_connection *c, void *data, int len);
void h2d_http2_on_writable(struct h2d_connection *c);

int h2d_http2_response_headers(struct h2d_request *r);

void h2d_http2_response_body_packfix(struct h2d_request *r,
		uint8_t **p_buf_pos, int *p_buf_len);
int h2d_http2_response_body_pack(struct h2d_request *r, uint8_t *payload,
		int length, bool is_body_finished);

void h2d_http2_response_body_finish(struct h2d_request *r);

void h2d_http2_connection_init(struct h2d_connection *c);

void h2d_http2_init(void);

#endif
