#include "asynctest-tcp.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

#include <async/async.h>
#include <async/farewellstream.h>
#include <async/queuestream.h>
#include <async/stringstream.h>
#include <async/tcp_connection.h>

typedef struct {
    async_t *async;
    tcp_server_t *server;
    tcp_conn_t *conn, *sconn;
    queuestream_t *downstream, *upstream;
    int state, verdict;
} TCP_CONTEXT;

enum {
    TCP_INIT,
    TCP_HELLO,
    TCP_WORLD,
    TCP_CLOSING,
    TCP_CLOSED,
};

static void tcp_close_up(TCP_CONTEXT *context)
{
    tlog("tcp_close_up");
    context->upstream = NULL;
}

static void tcp_close_down(TCP_CONTEXT *context)
{
    tlog("tcp_close_down");
    context->downstream = NULL;
}

static void tcp_probe_up(TCP_CONTEXT *context);

static void tcp_probe_up_init(TCP_CONTEXT *context)
{
    char buffer[100];
    ssize_t count = tcp_read(context->sconn, buffer, sizeof buffer);
    if (count >= 0) {
        tlog("Unexpected data received");
        async_quit_loop(context->async);
    } else if (errno != EAGAIN) {
        tlog("Unexpected error (errno %d)", (int) errno);
        async_quit_loop(context->async);
    }
}

static void tcp_probe_up_hello(TCP_CONTEXT *context)
{
    char buffer[100];
    ssize_t count = tcp_read(context->sconn, buffer, sizeof buffer);
    if (count < 0 && errno == EAGAIN)
        return;
    if (count != 5) {
        tlog("Unexpected error %d (errno %d)", (int) count, (int) errno);
        async_quit_loop(context->async);
        return;
    }
    if (strncmp(buffer, "Hello", 5) != 0) {
        tlog("Bad bytes from client");
        async_quit_loop(context->async);
        return;
    }
    if (context->upstream == NULL) {
        tlog("No upstream");
        async_quit_loop(context->async);
        return;
    }
    int fd = 0;
    count = tcp_send_ancillary_data(context->sconn, SOL_SOCKET, SCM_RIGHTS, &fd,
                                    sizeof fd);
    if (count != sizeof fd) {
        tlog("Failed to send credentials in ancillary data");
        async_quit_loop(context->async);
        return;
    }
    stringstream_t *msg = open_stringstream(context->async, "world");
    queuestream_enqueue(context->upstream, stringstream_as_bytestream_1(msg));
    context->state = TCP_WORLD;
    async_execute(context->async, (action_1) { context, (act_1) tcp_probe_up });
}

static void tcp_probe_up_world(TCP_CONTEXT *context)
{
    char buffer[100];
    ssize_t count = tcp_read(context->sconn, buffer, sizeof buffer);
    if (count >= 0) {
        tlog("Unexpected data received");
        async_quit_loop(context->async);
    } else if (errno != EAGAIN) {
        tlog("Unexpected error (errno %d)", (int) errno);
        async_quit_loop(context->async);
    }
}

static void tcp_probe_up_closing(TCP_CONTEXT *context)
{
    char buffer[100];
    ssize_t count = tcp_read(context->sconn, buffer, sizeof buffer);
    if (count < 0 && errno == EAGAIN)
        return;
    if (count != 0) {
        tlog("Unexpected error %d (errno %d)", (int) count, (int) errno);
        async_quit_loop(context->async);
        return;
    }
    if (context->upstream == NULL) {
        tlog("No upstream");
        async_quit_loop(context->async);
        return;
    }
    queuestream_terminate(context->upstream);
    tcp_close_input_stream(context->sconn);
    context->state = TCP_CLOSED;
}

static void tcp_probe_up(TCP_CONTEXT *context)
{
    switch (context->state) {
        case TCP_INIT:
            tcp_probe_up_init(context);
            break;
        case TCP_HELLO:
            tcp_probe_up_hello(context);
            break;
        case TCP_WORLD:
            tcp_probe_up_world(context);
            break;
        case TCP_CLOSING:
            tcp_probe_up_closing(context);
            break;
        case TCP_CLOSED: /* spurious */
            break;
        default:
            assert(0);
    }
}

static void tcp_service(TCP_CONTEXT *context)
{
    tcp_conn_t *sconn = tcp_accept(context->server, NULL, NULL);
    if (sconn == NULL)
        return;
    if (context->sconn != NULL) {
        tlog("Two connections accepted");
        async_quit_loop(context->async);
        return;
    }
    context->sconn = sconn;
    context->upstream = make_queuestream(context->async);
    farewellstream_t *fwstr =
        open_farewellstream(context->async,
                            queuestream_as_bytestream_1(context->upstream),
                            (action_1) { context, (act_1) tcp_close_up });
    tcp_set_output_stream(sconn, farewellstream_as_bytestream_1(fwstr));
    action_1 probe_up = { context, (act_1) tcp_probe_up };
    tcp_register_callback(sconn, probe_up);
    async_execute(context->async, probe_up);
}

static void tcp_probe_down(TCP_CONTEXT *context);

static void tcp_probe_down_init(TCP_CONTEXT *context)
{
    char buffer[100];
    ssize_t count = tcp_read(context->conn, buffer, sizeof buffer);
    if (count >= 0) {
        tlog("Unexpected data received");
        async_quit_loop(context->async);
        return;
    }
    if (errno != EAGAIN) {
        tlog("Unexpected error (errno %d)", (int) errno);
        async_quit_loop(context->async);
        return;
    }
    if (context->downstream == NULL) {
        tlog("No downstream");
        async_quit_loop(context->async);
        return;
    }
    stringstream_t *msg = open_stringstream(context->async, "Hello");
    queuestream_enqueue(context->downstream, stringstream_as_bytestream_1(msg));
    context->state = TCP_HELLO;
}

static void tcp_probe_down_hello(TCP_CONTEXT *context)
{
    char buffer[100];
    ssize_t count = tcp_read(context->conn, buffer, sizeof buffer);
    if (count >= 0) {
        tlog("Unexpected data received");
        async_quit_loop(context->async);
    } else if (errno != EAGAIN) {
        tlog("Unexpected error (errno %d)", (int) errno);
        async_quit_loop(context->async);
    }
}

static void tcp_probe_down_world(TCP_CONTEXT *context)
{
    char buffer[100];
    ssize_t count = tcp_read(context->conn, buffer, sizeof buffer);
    if (count < 0 && errno == EAGAIN)
        return;
    if (count != 5) {
        tlog("Unexpected error %d (errno %d)", (int) count, (int) errno);
        async_quit_loop(context->async);
        return;
    }
    if (strncmp(buffer, "world", 5) != 0) {
        tlog("Bad bytes from server");
        async_quit_loop(context->async);
        return;
    }
    if (context->upstream == NULL) {
        tlog("No upstream");
        async_quit_loop(context->async);
        return;
    }
    int level = 999, type = 999, fd = 999;
    count = tcp_peek_ancillary_data(context->conn, &level, &type);
    if (count < 0) {
        tlog("No ancillary data received");
        async_quit_loop(context->async);
        return;
    }
    if (level != SOL_SOCKET || type != SCM_RIGHTS) {
        tlog("Weird ancillary data received");
        async_quit_loop(context->async);
        return;
    }
    if (count != sizeof fd) {
        tlog("Bad ancillary data size detected");
        async_quit_loop(context->async);
        return;
    }
    count = tcp_recv_ancillary_data(context->conn, &fd, sizeof fd);
    if (count != sizeof fd) {
        tlog("Bad ancillary data size received");
        async_quit_loop(context->async);
        return;
    }
    int status = close(fd);
    if (status < 0) {
        tlog("Bad file descriptor received");
        async_quit_loop(context->async);
        return;
    }
    queuestream_terminate(context->downstream);
    context->state = TCP_CLOSING;
    async_execute(context->async,
                  (action_1) { context, (act_1) tcp_probe_down });
}

static void tcp_probe_down_closing(TCP_CONTEXT *context)
{
    char buffer[100];
    ssize_t count = tcp_read(context->conn, buffer, sizeof buffer);
    if (count >= 0) {
        tlog("Unexpected data received");
        async_quit_loop(context->async);
    } else if (errno != EAGAIN) {
        tlog("Unexpected error (errno %d)", (int) errno);
        async_quit_loop(context->async);
    }
}

static void tcp_probe_down_closed(TCP_CONTEXT *context)
{
    char buffer[100];
    ssize_t count = tcp_read(context->conn, buffer, sizeof buffer);
    if (count < 0 && errno == EAGAIN)
        return;
    if (count != 0) {
        tlog("Unexpected error %d (errno %d)", (int) count, (int) errno);
        async_quit_loop(context->async);
        return;
    }
    tcp_close_input_stream(context->conn);
    context->verdict = PASS;
    action_1 quit = { context->async, (act_1) async_quit_loop };
    async_execute(context->async, quit);
}

static void tcp_probe_down(TCP_CONTEXT *context)
{
    switch (context->state) {
        case TCP_INIT:
            tcp_probe_down_init(context);
            break;
        case TCP_HELLO:
            tcp_probe_down_hello(context);
            break;
        case TCP_WORLD:
            tcp_probe_down_world(context);
            break;
        case TCP_CLOSING:
            tcp_probe_down_closing(context);
            break;
        case TCP_CLOSED:
            tcp_probe_down_closed(context);
            break;
        default:
            assert(0);
    }
}

VERDICT test_tcp_connection(void)
{
    TCP_CONTEXT context = {
        .state = TCP_INIT,
        .verdict = FAIL,
    };
    async_t *async = context.async = make_async();
    const char *sockpath = "/tmp/asynctest.sock";
    (void) unlink(sockpath);
    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
    };
    strcpy(addr.sun_path, sockpath);
    tcp_server_t *server = context.server =
        tcp_listen(async, (struct sockaddr *) &addr, sizeof addr);
    if (server == NULL) {
        tlog("Unexpected error (errno %d) from tcp_listen", (int) errno);
        return FAIL;
    }
    async_execute(async, (action_1) { &context, (act_1) tcp_service });
    tcp_register_server_callback(server,
                                 (action_1) { &context, (act_1) tcp_service });
    tcp_conn_t *conn = context.conn =
        tcp_connect(async, NULL, (struct sockaddr *) &addr, sizeof addr);
    if (conn == NULL) {
        tlog("Could not connect (errno %d)", (int) errno);
        return FAIL;
    }
    context.downstream = make_queuestream(async);
    farewellstream_t *fwstr =
        open_farewellstream(async,
                            queuestream_as_bytestream_1(context.downstream),
                            (action_1) { &context, (act_1) tcp_close_down });
    tcp_set_output_stream(conn, farewellstream_as_bytestream_1(fwstr));
    action_1 probe_down = { &context, (act_1) tcp_probe_down };
    tcp_register_callback(conn, probe_down);
    async_execute(async, probe_down);
    async_timer_start(async, async_now(async) + 2 * ASYNC_S,
                      (action_1) { async, (act_1) async_quit_loop });
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    tcp_close(conn);
    if (context.sconn != NULL)
        tcp_close(context.sconn);
    int status = unlink(sockpath);
    assert(status >= 0);
    tcp_close_server(server);
    destroy_async(async);
    return posttest_check(context.verdict);
}
