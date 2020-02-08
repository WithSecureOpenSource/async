#ifndef __JSON_CONNECTION_H__
#define __JSON_CONNECTION_H__

#include "async.h"
#include "tcp_connection.h"
#include <encjson.h>

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
#endif

#endif
