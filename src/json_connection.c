#include "json_connection.h"

#include <assert.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"
#include "farewellstream.h"
#include "jsonencoder.h"
#include "jsonyield.h"
#include "naiveencoder.h"
#include "queuestream.h"

struct json_conn {
    async_t *async;
    uint64_t uid;
    tcp_conn_t *tcp_conn;
    queuestream_t *output_stream;
    jsonyield_t *input_stream;
};

static void output_closed(json_conn_t *conn)
{
    conn->output_stream = NULL;
}

FSTRACE_DECL(ASYNC_JSON_CONN_CREATE, "UID=%64u");

json_conn_t *open_json_conn(async_t *async, tcp_conn_t *tcp_conn,
                            size_t max_frame_size)
{
    json_conn_t *conn = fsalloc(sizeof *conn);
    conn->async = async;
    conn->uid = fstrace_get_unique_id();
    conn->tcp_conn = tcp_conn;
    conn->input_stream =
        open_jsonyield(async, tcp_get_input_stream(conn->tcp_conn),
                       max_frame_size);
    conn->output_stream = make_queuestream(async);
    bytestream_1 stream = queuestream_as_bytestream_1(conn->output_stream);
    action_1 farewell_cb = { conn, (act_1) output_closed };
    farewellstream_t *fws = open_farewellstream(async, stream, farewell_cb);
    tcp_set_output_stream(conn->tcp_conn, farewellstream_as_bytestream_1(fws));
    FSTRACE(ASYNC_JSON_CONN_CREATE, conn->uid);
    return conn;
}

FSTRACE_DECL(ASYNC_JSON_CONN_TERMINATE, "UID=%64u");

void json_conn_terminate(json_conn_t *conn)
{
    FSTRACE(ASYNC_JSON_CONN_TERMINATE, conn->uid);
    if (conn->output_stream)
        queuestream_terminate(conn->output_stream);
}

FSTRACE_DECL(ASYNC_JSON_CONN_CLOSE, "UID=%64u");

void json_conn_close(json_conn_t *conn)
{
    FSTRACE(ASYNC_JSON_CONN_CLOSE, conn->uid);
    assert(conn->async);
    jsonyield_close(conn->input_stream);
    tcp_close(conn->tcp_conn);
    async_wound(conn->async, conn);
    conn->async = NULL;
}

FSTRACE_DECL(ASYNC_JSON_CONN_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void json_conn_register_callback(json_conn_t *conn, action_1 action)
{
    FSTRACE(ASYNC_JSON_CONN_REGISTER, conn->uid, action.obj, action.act);
    jsonyield_register_callback(conn->input_stream, action);
}

FSTRACE_DECL(ASYNC_JSON_CONN_UNREGISTER, "UID=%64u");

void json_conn_unregister_callback(json_conn_t *conn)
{
    FSTRACE(ASYNC_JSON_CONN_UNREGISTER, conn->uid);
    jsonyield_register_callback(conn->input_stream, NULL_ACTION_1);
}

FSTRACE_DECL(ASYNC_JSON_CONN_SEND_DISCONNECTED, "UID=%64u");
FSTRACE_DECL(ASYNC_JSON_CONN_SEND, "UID=%64u MSG=%I");

void json_conn_send(json_conn_t *conn, json_thing_t *thing)
{
    if (!conn->output_stream) {
        FSTRACE(ASYNC_JSON_CONN_SEND_DISCONNECTED, conn->uid);
        return;
    }
    FSTRACE(ASYNC_JSON_CONN_SEND, conn->uid, json_trace, thing);
    bytestream_1 payload =
        jsonencoder_as_bytestream_1(json_encode(conn->async, thing));
    naiveencoder_t *naive_encoder =
        naive_encode(conn->async, payload, '\0', '\33');
    queuestream_enqueue(conn->output_stream,
                        naiveencoder_as_bytestream_1(naive_encoder));
}

FSTRACE_DECL(ASYNC_JSON_CONN_SEND_FD_DISCONNECTED, "UID=%64u");
FSTRACE_DECL(ASYNC_JSON_CONN_SEND_FD, "UID=%64u FD=%d");

int json_conn_send_fd(json_conn_t *conn, int fd, bool close_after_sending)
{
    if (!conn->output_stream) {
        FSTRACE(ASYNC_JSON_CONN_SEND_FD_DISCONNECTED, conn->uid);
        return -1;
    }
    FSTRACE(ASYNC_JSON_CONN_SEND_FD, conn->uid, fd);
    return tcp_send_fd(conn->tcp_conn, fd, close_after_sending);
}

FSTRACE_DECL(ASYNC_JSON_CONN_RECEIVE, "UID=%64u");

json_thing_t *json_conn_receive(json_conn_t *conn)
{
    FSTRACE(ASYNC_JSON_CONN_RECEIVE, conn->uid);
    return jsonyield_receive(conn->input_stream);
}
