#ifndef __TCP_CLIENT__
#define __TCP_CLIENT__

#include "async.h"
#include "fsadns.h"
#include "tcp_connection.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A TCP client is a convenience object that is used to resolve the
 * server's DNS name and establish a connection to any of the
 * alternative IPv4 and IPv6 addresses. The strategy is to initiate
 * connection establishment simultaneously to all alternatives.
 * Whichever attempt succeeds first is chosen as the connection while
 * the others are abandoned. */

typedef struct tcp_client tcp_client_t;

/* After open_tcp_client(), you should call tcp_client_establish(). The
 * server_host string is only needed for the duration of the call.
 *
 * If dns is NULL, synchronous, blocking name resolution is used. */
tcp_client_t *open_tcp_client_2(async_t *async, const char *server_host,
                                unsigned port, fsadns_t *dns);

/* Equivalent to open_tcp_client(async, server_host, port, NULL). */
tcp_client_t *open_tcp_client(async_t *async, const char *server_host,
                              unsigned port);

/* You can close a client before tcp_client_establish() has returned a
 * connection. */
void tcp_client_close(tcp_client_t *client);

/* Returns NULL and sets errno to EAGAIN until the connection has been
 * established. Other errno values indicate other permanent or
 * provisional errors. Once a non-NULL value is returned, the client
 * object ceases to be useful and can be closed. The returned connection
 * will not be affected by the closing but must eventually be closed by
 * the user. */
tcp_conn_t *tcp_client_establish(tcp_client_t *client);

/* Like tcp_set_output_stream (qv). To avoid race conditions, you should
 * set the output stream right after calling open_tcp_client(). It is
 * possible that the output stream's read or close method gets called
 * before the registered callback. If that should happen,
 * tcp_client_establish() *will* return a non-NULL TCP connection. */
void tcp_client_set_output_stream(tcp_client_t *client, bytestream_1 stream);

void tcp_client_register_callback(tcp_client_t *client, action_1 action);
void tcp_client_unregister_callback(tcp_client_t *client);

#ifdef __cplusplus
}
#endif

#endif
