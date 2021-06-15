#include "asynctest-jsonserver.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

#include <async/json_connection.h>
#include <async/jsonserver.h>

typedef struct {
    tester_base_t base;
    jsonserver_t *server;
    json_conn_t *conn;
} tester_t;

static void probe_server(tester_t *tester)
{
    jsonreq_t *req = jsonserver_receive_request(tester->server);
    if (!req) {
        if (errno != EAGAIN) {
            tlog("Errno %d from jsonserver_receive_request", errno);
            quit_test(&tester->base);
        }
        return;
    }
    json_thing_t *body = jsonreq_get_body(req);
    long long value;
    if (!json_cast_to_integer(body, &value) || value != 0) {
        quit_test(&tester->base);
        return;
    }
    json_thing_t *thing = json_make_integer(1);
    jsonreq_respond(req, thing);
    json_destroy_thing(thing);
}

static void probe_conn(tester_t *tester)
{
    json_thing_t *thing = json_conn_receive(tester->conn);
    if (!thing) {
        if (errno != EAGAIN) {
            tlog("Errno %d from json_conn_receive", errno);
            quit_test(&tester->base);
        }
        return;
    }
    long long value;
    if (json_cast_to_integer(thing, &value) && value == 1)
        tester->base.verdict = PASS;
    json_destroy_thing(thing);
    quit_test(&tester->base);
}

VERDICT test_jsonserver(void)
{
    async_t *async = make_async();
    tester_t tester = {};
    init_test(&tester.base, async, 10);
    const char *sockpath = "/tmp/asynctest.sock";
    (void) unlink(sockpath);
    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
    };
    strcpy(addr.sun_path, sockpath);

    tcp_server_t *server =
        tcp_listen(async, (struct sockaddr *) &addr, sizeof addr);
    if (server == NULL) {
        tlog("Unexpected error (errno %d) from tcp_listen", (int) errno);
        return FAIL;
    }
    tester.server = open_jsonserver(async, server, -1);
    action_1 server_cb = { &tester, (act_1) probe_server };
    jsonserver_register_callback(tester.server, server_cb);
    async_execute(async, server_cb);

    tcp_conn_t *conn =
        tcp_connect(async, NULL, (struct sockaddr *) &addr, sizeof addr);
    if (conn == NULL) {
        tlog("Could not connect (errno %d)", (int) errno);
        return FAIL;
    }
    tester.conn = open_json_conn(async, conn, -1);
    action_1 conn_cb = { &tester, (act_1) probe_conn };
    json_conn_register_callback(tester.conn, conn_cb);
    async_execute(async, conn_cb);

    json_thing_t *thing = json_make_integer(0);
    json_conn_send(tester.conn, thing);
    json_destroy_thing(thing);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    json_conn_close(tester.conn);
    int status = unlink(sockpath);
    assert(status >= 0);
    jsonserver_close(tester.server);
    destroy_async(async);
    return posttest_check(tester.base.verdict);
}
