#include "jsonyield.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

#include <fsdyn/bytearray.h>
#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"
#include "naiveframer.h"

enum {
    TERMINATOR = '\0',
    ESCAPE = '\33',
};

typedef enum {
    JSONYIELD_RECEIVING,
    JSONYIELD_READING,
    JSONYIELD_SKIPPING,
    JSONYIELD_CLOSED
} jsonyield_state_t;

struct jsonyield {
    async_t *async;
    uint64_t uid;
    naiveframer_t *framer;
    action_1 callback;
    jsonyield_state_t state;
    bytestream_1 *frame;
    byte_array_t *buffer;
};

FSTRACE_DECL(ASYNC_JSONYIELD_CREATE,
             "UID=%64u PTR=%p ASYNC=%p SOURCE=%p MAX-SIZE=%z");

jsonyield_t *open_jsonyield(async_t *async, bytestream_1 source,
                            size_t max_frame_size)
{
    jsonyield_t *yield = fsalloc(sizeof *yield);
    yield->async = async;
    yield->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_JSONYIELD_CREATE, yield->uid, yield, async, source.obj,
            max_frame_size);
    yield->framer = open_naiveframer(async, source, TERMINATOR, ESCAPE);
    yield->callback = NULL_ACTION_1;
    yield->state = JSONYIELD_RECEIVING;
    yield->buffer = make_byte_array(max_frame_size);
    return yield;
}

static const char *trace_state(void *pstate)
{
    switch (*(jsonyield_state_t *) pstate) {
        case JSONYIELD_RECEIVING:
            return "JSONYIELD_RECEIVING";
        case JSONYIELD_READING:
            return "JSONYIELD_READING";
        case JSONYIELD_SKIPPING:
            return "JSONYIELD_SKIPPING";
        case JSONYIELD_CLOSED:
            return "JSONYIELD_CLOSED";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNC_JSONYIELD_SET_STATE, "UID=%64u OLD=%I NEW=%I");

static void set_yield_state(jsonyield_t *yield, jsonyield_state_t state)
{
    FSTRACE(ASYNC_JSONYIELD_SET_STATE, yield->uid, trace_state, &yield->state,
            trace_state, &state);
    yield->state = state;
}

static ssize_t read_frame(void *obj, void *buf, size_t count)
{
    bytestream_1 *stream = obj;
    return bytestream_1_read(*stream, buf, count);
}

FSTRACE_DECL(ASYNC_JSONYIELD_INPUT_DUMP, "UID=%64u TEXT=%A");

static json_thing_t *do_receive(jsonyield_t *yield)
{
    switch (yield->state) {
        case JSONYIELD_RECEIVING:
            yield->frame = naiveframer_receive(yield->framer);
            if (!yield->frame)
                return NULL;
            set_yield_state(yield, JSONYIELD_READING);
            byte_array_clear(yield->buffer);
            bytestream_1_register_callback(*yield->frame, yield->callback);
            return jsonyield_receive(yield);
        case JSONYIELD_READING: {
            size_t read_pos = byte_array_size(yield->buffer);
            ssize_t count = byte_array_append_stream(yield->buffer, read_frame,
                                                     yield->frame, 1024);
            if (count < 0 && errno == ENOSPC) {
                char c;
                count = bytestream_1_read(*yield->frame, &c, 1);
                if (count > 0) {
                    set_yield_state(yield, JSONYIELD_SKIPPING);
                    errno = EMSGSIZE;
                    return NULL;
                }
            }
            if (count < 0)
                return NULL;
            if (!count) {
                bytestream_1_close(*yield->frame);
                set_yield_state(yield, JSONYIELD_RECEIVING);
                json_thing_t *thing =
                    json_utf8_decode(byte_array_data(yield->buffer),
                                     byte_array_size(yield->buffer));
                if (!thing)
                    errno = EILSEQ;
                return thing;
            }
            FSTRACE(ASYNC_JSONYIELD_INPUT_DUMP, yield->uid,
                    byte_array_data(yield->buffer) + read_pos, count);
            async_execute(yield->async, yield->callback);
            errno = EAGAIN;
            return NULL;
        }
        case JSONYIELD_SKIPPING: {
            char buffer[1024];
            ssize_t count =
                bytestream_1_read(*yield->frame, buffer, sizeof buffer);
            if (count < 0)
                return NULL;
            if (!count) {
                bytestream_1_close(*yield->frame);
                set_yield_state(yield, JSONYIELD_RECEIVING);
                return jsonyield_receive(yield);
            }
            async_execute(yield->async, yield->callback);
            errno = EAGAIN;
            return NULL;
        }
        default:
            errno = EBADF;
            return NULL;
    }
}

FSTRACE_DECL(ASYNC_JSONYIELD_RECEIVE, "UID=%64u THING=%p ERRNO=%e");

json_thing_t *jsonyield_receive(jsonyield_t *yield)
{
    json_thing_t *thing = do_receive(yield);
    FSTRACE(ASYNC_JSONYIELD_RECEIVE, yield->uid, thing);
    return thing;
}

static void *_receive(void *obj)
{
    return jsonyield_receive(obj);
}

void jsonyield_close(jsonyield_t *yield)
{
    assert(yield->state != JSONYIELD_CLOSED);
    naiveframer_close(yield->framer);
    yield->callback = NULL_ACTION_1;
    destroy_byte_array(yield->buffer);
    async_wound(yield->async, yield);
    set_yield_state(yield, JSONYIELD_CLOSED);
}

static void _close(void *obj)
{
    jsonyield_close(obj);
}

void jsonyield_register_callback(jsonyield_t *yield, action_1 action)
{
    yield->callback = action;
    naiveframer_register_callback(yield->framer, action);
    switch (yield->state) {
        case JSONYIELD_READING:
        case JSONYIELD_SKIPPING:
            bytestream_1_register_callback(*yield->frame, yield->callback);
            break;
        default:;
    }
}

static void _register_callback(void *obj, action_1 action)
{
    jsonyield_register_callback(obj, action);
}

void jsonyield_unregister_callback(jsonyield_t *yield)
{
    jsonyield_register_callback(yield, NULL_ACTION_1);
}

static void _unregister_callback(void *obj)
{
    jsonyield_unregister_callback(obj);
}

static const struct yield_1_vt jsonyield_vt = {
    .receive = _receive,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

yield_1 jsonyield_as_yield_1(jsonyield_t *yield)
{
    return (yield_1) { yield, &jsonyield_vt };
}
