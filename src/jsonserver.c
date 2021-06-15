#include "jsonserver.h"

#include <assert.h>
#include <errno.h>

#include <fsdyn/fsalloc.h>
#include <fsdyn/list.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"
#include "farewellstream.h"
#include "jsonencoder.h"
#include "jsonyield.h"
#include "naiveencoder.h"
#include "queuestream.h"

typedef enum {
    CONN_OPEN,
    CONN_CLOSING,
    CONN_ZOMBIE,
} conn_state_t;

typedef struct conn {
    jsonserver_t *server;
    uint64_t uid;
    conn_state_t state;
    list_elem_t *loc;
    tcp_conn_t *tcp_conn;
    queuestream_t *output_stream;
    jsonyield_t *input_stream;
    bool input_closed;
    uint32_t reference_count;
} conn_t;

struct jsonreq {
    conn_t *conn;
    json_thing_t *body;
};

struct jsonserver {
    async_t *async;
    uint64_t uid;
    tcp_server_t *tcp_server;
    size_t max_frame_size;
    list_t *connections;
    list_t *pending;
    action_1 callback;
};

static jsonreq_t *create_jsonreq(conn_t *conn, json_thing_t *body)
{
    jsonreq_t *request = fsalloc(sizeof *request);
    request->conn = conn;
    request->body = body;
    conn->reference_count++;
    return request;
}

static void jsonreq_destroy(jsonreq_t *request)
{
    request->conn->reference_count--;
    json_destroy_thing(request->body);
    fsfree(request);
}

static const char *conn_trace_state(conn_state_t *state)
{
    switch (*state) {
        case CONN_OPEN:
            return "CONN_OPEN";
        case CONN_CLOSING:
            return "CONN_CLOSING";
        case CONN_ZOMBIE:
            return "CONN_ZOMBIE";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNC_JSONSERVER_CONN_SET_STATE, "UID=%64u OLD=%I NEW=%I");

static void conn_set_state(conn_t *conn, conn_state_t state)
{
    FSTRACE(ASYNC_JSONSERVER_CONN_SET_STATE, conn->uid, conn_trace_state,
            &conn->state, conn_trace_state, &state);
    conn->state = state;
}

static void conn_terminate(conn_t *conn)
{
    conn_set_state(conn, CONN_CLOSING);
    queuestream_terminate(conn->output_stream);
}

FSTRACE_DECL(ASYNC_JSONSERVER_CONN_PROBE, "UID=%64u REQ=%p");
FSTRACE_DECL(ASYNC_JSONSERVER_CONN_PROBE_SPURIOUS, "UID=%64u");
FSTRACE_DECL(ASYNC_JSONSERVER_CONN_READ_EOF, "UID=%64u");
FSTRACE_DECL(ASYNC_JSONSERVER_CONN_READ_FAIL, "UID=%64u ERRNO=%e");

static void conn_probe(conn_t *conn)
{
    switch (conn->state) {
        case CONN_OPEN:
            break;
        default:
            return;
    }
    json_thing_t *thing = jsonyield_receive(conn->input_stream);
    if (!thing) {
        if (errno == EAGAIN) {
            FSTRACE(ASYNC_JSONSERVER_CONN_PROBE_SPURIOUS, conn->uid);
            return;
        }
        if (errno == 0)
            FSTRACE(ASYNC_JSONSERVER_CONN_READ_EOF, conn->uid);
        else
            FSTRACE(ASYNC_JSONSERVER_CONN_READ_FAIL, conn->uid);
        conn->input_closed = true;
        if (conn->reference_count == 0)
            conn_terminate(conn);
        return;
    }
    jsonreq_t *request = create_jsonreq(conn, thing);
    list_append(conn->server->pending, request);
    FSTRACE(ASYNC_JSONSERVER_CONN_PROBE, conn->uid, request);
    async_execute(conn->server->async, conn->server->callback);
    async_execute(conn->server->async, (action_1) { conn, (act_1) conn_probe });
}

static void conn_close(conn_t *conn)
{
    conn_set_state(conn, CONN_ZOMBIE);
    list_remove(conn->server->connections, conn->loc);
    queuestream_release(conn->output_stream);
    jsonyield_close(conn->input_stream);
    tcp_close(conn->tcp_conn);
    async_wound(conn->server->async, conn);
}

FSTRACE_DECL(ASYNC_JSONSERVER_CONN_OUTPUT_CLOSED, "UID=%64u");

static void conn_output_closed(conn_t *conn)
{
    switch (conn->state) {
        case CONN_OPEN:
        case CONN_CLOSING:
            break;
        case CONN_ZOMBIE:
            return;
    }
    FSTRACE(ASYNC_JSONSERVER_CONN_OUTPUT_CLOSED, conn->uid);
    if (conn->reference_count == 0)
        conn_close(conn);
}

FSTRACE_DECL(ASYNC_JSONSERVER_CONN_CREATE, "UID=%64u SERVER-ID=%64u");

static void open_connection(jsonserver_t *server, tcp_conn_t *tcp_conn)
{
    conn_t *conn = fsalloc(sizeof *conn);
    conn->server = server;
    conn->uid = fstrace_get_unique_id();
    conn->state = CONN_OPEN;
    conn->input_closed = false;
    conn->reference_count = 0;
    conn->loc = list_append(server->connections, conn);
    conn->tcp_conn = tcp_conn;
    conn->output_stream = make_relaxed_queuestream(server->async);
    bytestream_1 stream = queuestream_as_bytestream_1(conn->output_stream);
    action_1 farewell_cb = { conn, (act_1) conn_output_closed };
    farewellstream_t *fws =
        open_relaxed_farewellstream(server->async, stream, farewell_cb);
    tcp_set_output_stream(conn->tcp_conn, farewellstream_as_bytestream_1(fws));
    conn->input_stream =
        open_jsonyield(server->async, tcp_get_input_stream(conn->tcp_conn),
                       server->max_frame_size);
    action_1 read_cb = { conn, (act_1) conn_probe };
    jsonyield_register_callback(conn->input_stream, read_cb);
    async_execute(server->async, read_cb);
    FSTRACE(ASYNC_JSONSERVER_CONN_CREATE, conn->uid, server->uid);
}

FSTRACE_DECL(ASYNC_JSONSERVER_ACCEPT_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_JSONSERVER_PROBE_SPURIOUS, "UID=%64u");

static void jsonserver_probe(jsonserver_t *server)
{
    if (!server->async)
        return;
    tcp_conn_t *tcp_conn = tcp_accept(server->tcp_server, NULL, NULL);
    if (!tcp_conn) {
        if (errno == EAGAIN) {
            FSTRACE(ASYNC_JSONSERVER_PROBE_SPURIOUS, server->uid);
            return;
        }
        FSTRACE(ASYNC_JSONSERVER_ACCEPT_FAIL, server->uid);
        return;
    }
    open_connection(server, tcp_conn);
    async_execute(server->async,
                  (action_1) { server, (act_1) jsonserver_probe });
}

FSTRACE_DECL(ASYNC_JSONSERVER_CREATE, "UID=%64u");

jsonserver_t *open_jsonserver(async_t *async, tcp_server_t *tcp_server,
                              size_t max_frame_size)
{
    jsonserver_t *server = fsalloc(sizeof *server);
    server->async = async;
    server->uid = fstrace_get_unique_id();
    server->tcp_server = tcp_server;
    server->max_frame_size = max_frame_size;
    server->connections = make_list();
    server->pending = make_list();
    action_1 server_cb = { server, (act_1) jsonserver_probe };
    tcp_register_server_callback(tcp_server, server_cb);
    FSTRACE(ASYNC_JSONSERVER_CREATE, server->uid);
    return server;
}

FSTRACE_DECL(ASYNC_JSONSERVER_CLOSE, "UID=%64u");

void jsonserver_close(jsonserver_t *server)
{
    FSTRACE(ASYNC_JSONSERVER_CLOSE, server->uid);
    list_foreach(server->pending, (void *) jsonreq_destroy, NULL);
    destroy_list(server->pending);
    while (!list_empty(server->connections)) {
        list_elem_t *element = list_get_first(server->connections);
        conn_close((conn_t *) list_elem_get_value(element));
    }
    destroy_list(server->connections);
    tcp_close_server(server->tcp_server);
    async_wound(server->async, server);
    server->async = NULL;
}

FSTRACE_DECL(ASYNC_JSONSERVER_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void jsonserver_register_callback(jsonserver_t *server, action_1 action)
{
    FSTRACE(ASYNC_JSONSERVER_REGISTER, server->uid, action.obj, action.act);
    server->callback = action;
}

FSTRACE_DECL(ASYNC_JSONSERVER_UNREGISTER, "UID=%64u");

void jsonserver_unregister_callback(jsonserver_t *server)
{
    FSTRACE(ASYNC_JSONSERVER_UNREGISTER, server->uid);
    server->callback = NULL_ACTION_1;
}

FSTRACE_DECL(ASYNC_JSONSERVER_RECEIVE_REQUEST_SPURIOUS, "UID=%64u");
FSTRACE_DECL(ASYNC_JSONSERVER_RECEIVE_REQUEST, "UID=%64u CONN-ID=%64u REQ=%p");

jsonreq_t *jsonserver_receive_request(jsonserver_t *server)
{
    jsonreq_t *request = (jsonreq_t *) list_pop_first(server->pending);
    if (!request) {
        FSTRACE(ASYNC_JSONSERVER_RECEIVE_REQUEST_SPURIOUS, server->uid);
        errno = EAGAIN;
        return NULL;
    }
    FSTRACE(ASYNC_JSONSERVER_RECEIVE_REQUEST, server->uid, request->conn->uid,
            request);
    return request;
}

json_thing_t *jsonreq_get_body(jsonreq_t *request)
{
    return request->body;
}

FSTRACE_DECL(ASYNC_JSONREQ_RESPOND, "CONN-ID=%64u REQ=%p RESPONSE=%I");
FSTRACE_DECL(ASYNC_JSONREQ_RESPOND_DISCONNECTED,
             "CONN-ID=%64u REQ=%p RESPONSE=%I");

void jsonreq_respond(jsonreq_t *request, json_thing_t *body)
{
    conn_t *conn = request->conn;
    assert(conn->state == CONN_OPEN);
    jsonreq_destroy(request);
    if (!queuestream_closed(conn->output_stream)) {
        FSTRACE(ASYNC_JSONREQ_RESPOND, conn->uid, request, json_trace, body);
        bytestream_1 payload =
            jsonencoder_as_bytestream_1(json_encode(conn->server->async, body));
        naiveencoder_t *naive_encoder =
            naive_encode(conn->server->async, payload, '\0', '\33');
        queuestream_enqueue(conn->output_stream,
                            naiveencoder_as_bytestream_1(naive_encoder));
        if (conn->reference_count == 0 && conn->input_closed)
            conn_terminate(conn);
    } else {
        FSTRACE(ASYNC_JSONREQ_RESPOND_DISCONNECTED, conn->uid, request,
                json_trace, body);
        if (conn->reference_count == 0)
            conn_close(conn);
    }
}
