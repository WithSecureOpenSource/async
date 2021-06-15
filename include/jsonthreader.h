#ifndef ASYNC_JSONTHREADER_H
#define ASYNC_JSONTHREADER_H

#include <encjson.h>
#include <fsdyn/list.h>

#include "async.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jsonthreader jsonthreader_t;

/*
 * A threader object performs arbitrary tasks in a subprocess, with a
 * given degree of parallelism (threads). A task, encoded as a JSON
 * object, is dispatched using 'jsonthreader_send' and is processed by
 * the handler argument. If the handler return value is non-null, it
 * is sent as a response to the main process, and can be retrieved
 * using 'jsonthreader_receive'.
 */
jsonthreader_t *make_jsonthreader(
    async_t *async, list_t *keep_fds, action_1 post_fork_cb,
    json_thing_t *(*handler)(void *, json_thing_t *), void *obj,
    size_t max_frame_size, unsigned max_parallel);
void destroy_jsonthreader(jsonthreader_t *threader);

void jsonthreader_register_callback(jsonthreader_t *threader, action_1 action);
void jsonthreader_unregister_callback(jsonthreader_t *threader);

void jsonthreader_send(jsonthreader_t *threader, json_thing_t *thing);
json_thing_t *jsonthreader_receive(jsonthreader_t *threader);

void jsonthreader_terminate(jsonthreader_t *threader);

#ifdef __cplusplus
}
#endif

#endif
