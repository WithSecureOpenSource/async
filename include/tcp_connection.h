#ifndef __TCP_CONNECTION__
#define __TCP_CONNECTION__

/* This API is for any stream-oriented socket connections and servers.
 * The "tcp_" prefix is a historical relic.
 */

#include <stdbool.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <fsdyn/list.h>

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tcp_conn tcp_conn_t;
typedef struct tcp_server tcp_server_t;

enum {
    TCP_FLAG_INGRESS_LIVE = 0x01,
    TCP_FLAG_EGRESS_LIVE = 0x02,
    TCP_FLAG_EPOLL_RECV = 0x04,      /* recv returned EAGAIN */
    TCP_FLAG_EPOLL_SEND = 0x08,      /* send returned EAGAIN */
    TCP_FLAG_INGRESS_PENDING = 0x10, /* waiting for tcp_read to be called */
    TCP_FLAG_EGRESS_PENDING = 0x20,  /* wtng for notific'n from outstrm */
};

typedef struct {
    uint32_t flags;
    uint64_t bytes_received, bytes_sent;
    size_t bytes_to_be_sent;
} tcp_statistics_1;

tcp_conn_t *tcp_connect(async_t *async, const struct sockaddr *from,
                        const struct sockaddr *to, socklen_t addrlen);
tcp_conn_t *tcp_adopt_connection(async_t *async, int connfd);

void tcp_get_statistics_1(tcp_conn_t *conn, tcp_statistics_1 *stats);

/* Close the connection and release the resources associated with it.
 * If the output stream is still open, an immediate close is
 * performed. If the input stream is still open, it is shut down, but
 * the user is responsible for closing it eventually. */
void tcp_close(tcp_conn_t *conn);

/* Close one or both directions of the communication. The values for
 * 'how' are SHUT_RD, SHUT_WR or SHUT_RDWR (see shutdown(2)).
 *
 * If SHUT_WR or SHUT_RDWR is specified and the output stream is still
 * open, an immediate close is performed as though an EOF had been
 * read from the output stream.
 *
 * If SHUT_RD or SHUT_RDWR is specified and the input stream is still
 * open, an immediate close is performed. Note that if the input
 * stream is bound to another byte stream, it will eventually close
 * the input stream redundantly.
 *
 * Note that calling tcp_shut_down(SHUT_RDWR) is not the same as
 * calling tcp_close(conn). Even after both communication directions
 * are shut down, the connection object itself needs to be released
 * with tcp_close(). On the other hand, tcp_close() does make sure
 * both communication directions are shut down.
 *
 * Calling tcp_shut_down() is not considered part of typical usage.
 * Instead closing the input stream or delivering an EOF to the output
 * stream are normal ways to shut down communication. */
void tcp_shut_down(tcp_conn_t *conn, int how, int *perror);

/* Equivalent to:
 *
 *   bytestream_1_read(tcp_get_input_stream(conn), buf, count)
 *
 * The call
 *
 *   tcp_read(conn, NULL, 0)
 *
 * can be used to probe connection establishment. The function returns
 * a negative number with errno == EAGAIN during connection
 * establishment and 0 while the connection is established (half
 * open).
 */
ssize_t tcp_read(tcp_conn_t *conn, void *buf, size_t count);

/* Equivalent to:
 *
 *   bytestream_1_close(tcp_get_input_stream(conn))
 */
void tcp_close_input_stream(tcp_conn_t *conn);

/* Equivalent to:
 *
 *   bytestream_1_register_callback(tcp_get_input_stream(conn), action)
 */
void tcp_register_callback(tcp_conn_t *conn, action_1 action);

/* Equivalent to:
 *
 *   bytestream_1_unregister_callback(tcp_get_input_stream(conn))
 */
void tcp_unregister_callback(tcp_conn_t *conn);

/* Data bytes are received through the connection's input stream, which
 * is returned by this function. Closing the input stream corresponds to
 * calling tcp_shut_down(SHUT_RD) on the connection. That is, the
 * connection can still continue sending data. */
bytestream_1 tcp_get_input_stream(tcp_conn_t *conn);

/* The TCP connection reads data bytes from an external output stream,
 * which is set using this function. Reading 0 (EOF) or an error (other
 * than EAGAIN) from the stream triggers a call to
 * tcp_shut_down(SHUT_WR). */
void tcp_set_output_stream(tcp_conn_t *conn, bytestream_1 output_stream);

int tcp_get_fd(tcp_conn_t *conn);

/* Probe ancillary data without consuming it. Return a negative value
 * with errno = EAGAIN if no ancillary data has been received.
 * Otherwise, fill in level and type (see cmsg(3)) and return the number
 * of bytes in the next available ancillary data block.
 */
ssize_t tcp_peek_ancillary_data(tcp_conn_t *conn, int *level, int *type);

/* Read out ancillary data from the socket. If none is available, a
 * negative value is returned and errno is set to EAGAIN. Otherwise,
 * copy the next ancillary data block to buf, and return the number of
 * transferred bytes.
 *
 * If the caller-supplied buffer is too small to hold the value, the
 * data gets truncated and the remainder is discarded.
 */
ssize_t tcp_recv_ancillary_data(tcp_conn_t *conn, void *buf, size_t size);

/* Receive an open file descriptor as ancillary data. Only available
 * for local domain sockets. Call tcp_recv_fd() after receiving the
 * associated data message. Return a negative value in case of an
 * error. */
int tcp_recv_fd(tcp_conn_t *conn);

/* Receive a list of open file descriptors that have been received through
 * the tcp connection, but haven't been read out yet with tcp_recv_fd or
 * tcp_recv_ancillary_data.
 *
 * The ownership of the file descriptors remains in the tcp connection.
 * Therefore the returned file descriptors are still available to be received
 * with tcp_recv_fd and tcp_recv_ancillary_data and they should not be closed
 * in the receiver process when received with this method.
 *
 * One usage example for this method is when the receiver process forks. Then
 * it can get a list of received file descriptors that it should close in the
 * forked child process.
 *
 * Returned list_t contains elements of type integer_t from fsdyn/integer.h.
 *
 * Caller becomes owner of the returned list_t.
 */
list_t *tcp_peek_received_fds(tcp_conn_t *conn);

/* Submit ancillary data for delivery with the next outgoing message
 * bytes. If previous ancillary data is still pending, the new data is
 * merged with it.
 *
 * Returns size or, in case of an error, a negative value (setting
 * errno).
 */
ssize_t tcp_send_ancillary_data(tcp_conn_t *conn, int level, int type,
                                const void *buf, size_t size);

/* Send an open file descriptor as ancillary data. Only available for
 * local domain sockets. Call tcp_send_fd() before queuing the
 * associated data message for sending. Return a negative value in case
 * of an error. */
int tcp_send_fd(tcp_conn_t *conn, int fd, bool close_after_sending);

/* Schedule a callback to be executed once all ancillary data has been
 * delivered to the kernel. The tcp_send_ancillary_data() does not do
 * that immediately since ancillary data needs real data to be sent as
 * well.
 *
 * The callback takes place only once. The proper use is to call
 * tcp_mark_ancillary_data() every time a burst of
 * tcp_send_ancillary_data() calls is made (if such a notification is
 * desired).
 *
 * The only way to unregister the callback is by closing the connection,
 * in which case the callback is not triggered.
 *
 * All pending callbacks are scheduled for execution when tcp_close() is
 * called.
 */
void tcp_mark_ancillary_data(tcp_conn_t *conn, action_1 action);

tcp_server_t *tcp_listen(async_t *async, const struct sockaddr *address,
                         socklen_t addrlen);
void tcp_close_server(tcp_server_t *server);
void tcp_register_server_callback(tcp_server_t *server, action_1 action);
void tcp_unregister_server_callback(tcp_server_t *server);
tcp_conn_t *tcp_accept(tcp_server_t *server, struct sockaddr *addr,
                       socklen_t *addrlen);
int tcp_get_server_fd(tcp_server_t *server);
tcp_server_t *tcp_adopt_server(async_t *async, int serverfd);

#ifdef __cplusplus
}
#endif

#endif
