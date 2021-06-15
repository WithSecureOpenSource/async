#ifndef ASYNC_JSONSERVER_H
#define ASYNC_JSONSERVER_H

#include <encjson.h>

#include "tcp_connection.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jsonserver jsonserver_t;
typedef struct jsonreq jsonreq_t;

/* Create a json server over the given tcp server, supporting requests
 * up to the given maximum size. The server pipelines the requests and
 * it is up to the application to define an ordering by means of
 * transaction ids. */
jsonserver_t *open_jsonserver(async_t *async, tcp_server_t *tcp_server,
                              size_t max_frame_size);
void jsonserver_close(jsonserver_t *server);

/* The callback is invoked whenever a new request is available. */
void jsonserver_register_callback(jsonserver_t *server, action_1 action);
void jsonserver_unregister_callback(jsonserver_t *server);

/* Return a pending request. The request stays valid until the
 * application responds to it. If there are no pending requests, NULL
 * is returned and errno is set to EAGAIN. */
jsonreq_t *jsonserver_receive_request(jsonserver_t *server);
/* Return the body of the given request. The body stays valid until
 * the application responds to the request. */
json_thing_t *jsonreq_get_body(jsonreq_t *request);
void jsonreq_respond(jsonreq_t *request, json_thing_t *body);

#ifdef __cplusplus
}
#endif

#endif
