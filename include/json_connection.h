#ifndef __JSON_CONNECTION_H__
#define __JSON_CONNECTION_H__

#include <encjson.h>

#include "async.h"
#include "tcp_connection.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct json_conn json_conn_t;

/* this function acquires ownership of the tcp_conn_t object. */
json_conn_t *open_json_conn(async_t *async, tcp_conn_t *tcp_conn,
                            size_t max_frame_size);
void json_conn_terminate(json_conn_t *conn);
void json_conn_close(json_conn_t *conn);
void json_conn_register_callback(json_conn_t *conn, action_1 action);
void json_conn_unregister_callback(json_conn_t *conn);
void json_conn_send(json_conn_t *conn, json_thing_t *thing);
int json_conn_send_fd(json_conn_t *conn, int fd, bool close_after_sending);
json_thing_t *json_conn_receive(json_conn_t *conn);

#ifdef __cplusplus
}

#include <functional>
#include <memory>

namespace fsecure {
namespace async {

// std::unique_ptr for json_conn_t with custom deleter.
using JsonConnPtr =
    std::unique_ptr<json_conn_t, std::function<void(json_conn_t *)>>;

// Create JsonConnPtr that takes ownership of the provided json_conn_t. Pass
// nullptr to create an instance which doesn't contain any json_conn_t object.
inline JsonConnPtr make_json_conn_ptr(json_conn_t *conn)
{
    return { conn, json_conn_close };
}

} // namespace async
} // namespace fsecure

#endif

#endif
