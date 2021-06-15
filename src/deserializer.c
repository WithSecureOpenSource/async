#include "deserializer.h"

#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"
#include "queuestream.h"

typedef enum {
    DESERIALIZER_CLEAN_BREAK,    /* no outstanding frame */
    DESERIALIZER_READING_FRAME,  /* frame being processed */
    DESERIALIZER_SKIPPING_FRAME, /* user closed frame before exhausting it */
    DESERIALIZER_AFTER_FRAME,    /* frame exhausted but not closed */
    DESERIALIZER_EOF,            /* frame yield exhausted */
    DESERIALIZER_CLOSED
} deserializer_state_t;

struct deserializer {
    deserializer_state_t state;
    async_t *async;
    uint64_t uid;
    decoder_factory_t factory;
    void *factory_obj;

    /* for tracking deserializer_receive() only */
    action_1 callback;

    /* tracked by deserializer (DESERIALIZER_CLEAN_BREAK) */
    queuestream_t *source;

    /* tracked by deserializer (DESERIALIZER_SKIPPING_FRAME) */
    bytestream_2 decoder;

    /* returned to the user (DESERIALIZER_READING_FRAME) */
    bytestream_1 frame;
};

static const char *trace_state(void *pstate)
{
    switch (*(deserializer_state_t *) pstate) {
        case DESERIALIZER_CLEAN_BREAK:
            return "DESERIALIZER_CLEAN_BREAK";
        case DESERIALIZER_READING_FRAME:
            return "DESERIALIZER_READING_FRAME";
        case DESERIALIZER_SKIPPING_FRAME:
            return "DESERIALIZER_SKIPPING_FRAME";
        case DESERIALIZER_AFTER_FRAME:
            return "DESERIALIZER_AFTER_FRAME";
        case DESERIALIZER_EOF:
            return "DESERIALIZER_EOF";
        case DESERIALIZER_CLOSED:
            return "DESERIALIZER_CLOSED";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNC_DESERIALIZER_SET_STATE, "UID=%64u OLD=%I NEW=%I");

static void set_deserializer_state(deserializer_t *deserializer,
                                   deserializer_state_t state)
{
    FSTRACE(ASYNC_DESERIALIZER_SET_STATE, deserializer->uid, trace_state,
            &deserializer->state, trace_state, &state);
    deserializer->state = state;
}

static ssize_t do_read(deserializer_t *deserializer, void *buf, size_t count)
{
    switch (deserializer->state) {
        case DESERIALIZER_READING_FRAME: {
            if (count == 0)
                return 0;
            ssize_t n = bytestream_2_read(deserializer->decoder, buf, count);
            if (n)
                return n;
            set_deserializer_state(deserializer, DESERIALIZER_AFTER_FRAME);
            return 0;
        }
        case DESERIALIZER_AFTER_FRAME:
            return 0;
        default:
            assert(false);
    }
}

FSTRACE_DECL(ASYNC_DESERIALIZER_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_DESERIALIZER_READ_DUMP, "UID=%64u DATA=%B");

static ssize_t frame_read(void *obj, void *buf, size_t count)
{
    deserializer_t *deserializer = obj;
    ssize_t n = do_read(deserializer, buf, count);
    FSTRACE(ASYNC_DESERIALIZER_READ, deserializer->uid, count, n);
    FSTRACE(ASYNC_DESERIALIZER_READ_DUMP, deserializer->uid, buf, n);
    return n;
}

FSTRACE_DECL(ASYNC_DESERIALIZER_PROBE, "UID=%64u");
FSTRACE_DECL(ASYNC_DESERIALIZER_SPURIOUS_PROBE, "UID=%64u");

static void probe(deserializer_t *deserializer)
{
    switch (deserializer->state) {
        case DESERIALIZER_CLEAN_BREAK:
        case DESERIALIZER_SKIPPING_FRAME:
            FSTRACE(ASYNC_DESERIALIZER_PROBE, deserializer->uid);
            action_1_perf(deserializer->callback);
            break;
        default:
            FSTRACE(ASYNC_DESERIALIZER_SPURIOUS_PROBE, deserializer->uid);
    }
}

static void conclude_frame(deserializer_t *deserializer)
{
    queuestream_push_bytes(deserializer->source,
                           bytestream_2_leftover_bytes(deserializer->decoder),
                           bytestream_2_leftover_size(deserializer->decoder));
    bytestream_2_close(deserializer->decoder);
    action_1 probe_cb = { deserializer, (act_1) probe };
    queuestream_register_callback(deserializer->source, probe_cb);
    set_deserializer_state(deserializer, DESERIALIZER_CLEAN_BREAK);
}

FSTRACE_DECL(ASYNC_DESERIALIZER_CLOSE_FRAME, "UID=%64u");

static void frame_close(void *obj)
{
    deserializer_t *deserializer = obj;
    FSTRACE(ASYNC_DESERIALIZER_CLOSE_FRAME, deserializer->uid);
    switch (deserializer->state) {
        case DESERIALIZER_READING_FRAME: {
            action_1 probe_cb = { deserializer, (act_1) probe };
            bytestream_2_register_callback(deserializer->decoder, probe_cb);
            set_deserializer_state(deserializer, DESERIALIZER_SKIPPING_FRAME);
            break;
        }
        case DESERIALIZER_AFTER_FRAME:
            conclude_frame(deserializer);
            break;
        default:
            assert(false);
    }
}

FSTRACE_DECL(ASYNC_DESERIALIZER_REGISTER_FRAME, "UID=%64u OBJ=%p ACT=%p");

static void frame_register_callback(void *obj, action_1 action)
{
    deserializer_t *deserializer = obj;
    FSTRACE(ASYNC_DESERIALIZER_REGISTER_FRAME, deserializer->uid, action.obj,
            action.act);
    if (deserializer->state == DESERIALIZER_READING_FRAME)
        bytestream_2_register_callback(deserializer->decoder, action);
}

FSTRACE_DECL(ASYNC_DESERIALIZER_UNREGISTER_FRAME, "UID=%64u");

static void frame_unregister_callback(void *obj)
{
    deserializer_t *deserializer = obj;
    FSTRACE(ASYNC_DESERIALIZER_UNREGISTER_FRAME, deserializer->uid);
    if (deserializer->state == DESERIALIZER_READING_FRAME)
        bytestream_2_unregister_callback(deserializer->decoder);
}

static const struct bytestream_1_vt frame_vt = {
    .read = frame_read,
    .close = frame_close,
    .register_callback = frame_register_callback,
    .unregister_callback = frame_unregister_callback
};

FSTRACE_DECL(ASYNC_DESERIALIZER_CREATE, "UID=%64u PTR=%p ASYNC=%p SOURCE=%p");

deserializer_t *open_deserializer(async_t *async, bytestream_1 source,
                                  decoder_factory_t factory, void *factory_obj)
{
    deserializer_t *deserializer = fsalloc(sizeof *deserializer);
    deserializer->state = DESERIALIZER_CLEAN_BREAK;
    deserializer->async = async;
    deserializer->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_DESERIALIZER_CREATE, deserializer->uid, deserializer, async,
            source.obj);
    deserializer->callback = NULL_ACTION_1;
    deserializer->source = make_queuestream(async);
    deserializer->factory = factory;
    deserializer->factory_obj = factory_obj;
    queuestream_enqueue(deserializer->source, source);
    queuestream_terminate(deserializer->source);
    action_1 probe_cb = { deserializer, (act_1) probe };
    queuestream_register_callback(deserializer->source, probe_cb);
    deserializer->frame = (bytestream_1) { deserializer, &frame_vt };
    return deserializer;
}

static bytestream_1 *receive_at_clean_break(deserializer_t *deserializer)
{
    uint8_t peek;
    switch (queuestream_read(deserializer->source, &peek, 1)) {
        case 0:
            set_deserializer_state(deserializer, DESERIALIZER_EOF);
            errno = 0;
            return NULL;
        default:
            return NULL;
        case 1:;
    }
    queuestream_push_bytes(deserializer->source, &peek, 1);
    bytestream_1 source = queuestream_as_bytestream_1(deserializer->source);
    deserializer->decoder =
        deserializer->factory(deserializer->factory_obj, source);
    queuestream_unregister_callback(deserializer->source);
    set_deserializer_state(deserializer, DESERIALIZER_READING_FRAME);
    return &deserializer->frame;
}

static bytestream_1 *receive_skipping_frame(deserializer_t *deserializer)
{
    uint8_t buf[2000];
    size_t count = sizeof buf;
    ssize_t n = bytestream_2_read(deserializer->decoder, buf, count);
    if (n < 0)
        return NULL;
    if (n == 0) {
        conclude_frame(deserializer);
        return deserializer_receive(deserializer);
    }
    async_execute(deserializer->async, deserializer->callback);
    errno = EAGAIN;
    return NULL;
}

static bytestream_1 *do_receive(deserializer_t *deserializer)
{
    switch (deserializer->state) {
        case DESERIALIZER_CLEAN_BREAK:
            return receive_at_clean_break(deserializer);
        case DESERIALIZER_READING_FRAME:
        case DESERIALIZER_AFTER_FRAME:
            errno = EAGAIN;
            return NULL;
        case DESERIALIZER_SKIPPING_FRAME:
            return receive_skipping_frame(deserializer);
        case DESERIALIZER_EOF:
            errno = 0;
            return NULL;
        default:
            errno = EBADF;
            return NULL;
    }
}

FSTRACE_DECL(ASYNC_DESERIALIZER_RECEIVE, "UID=%64u FRAME=%p ERRNO=%e");

bytestream_1 *deserializer_receive(deserializer_t *deserializer)
{
    bytestream_1 *frame = do_receive(deserializer);
    FSTRACE(ASYNC_DESERIALIZER_RECEIVE, deserializer->uid, frame);
    return frame;
}

static void *_receive(void *obj)
{
    return deserializer_receive(obj);
}

FSTRACE_DECL(ASYNC_DESERIALIZER_CLOSE, "UID=%64u");

void deserializer_close(deserializer_t *deserializer)
{
    FSTRACE(ASYNC_DESERIALIZER_CLOSE, deserializer->uid);
    switch (deserializer->state) {
        case DESERIALIZER_CLEAN_BREAK:
        case DESERIALIZER_EOF:
            break;
        case DESERIALIZER_READING_FRAME:
        case DESERIALIZER_SKIPPING_FRAME:
        case DESERIALIZER_AFTER_FRAME:
            bytestream_2_close(deserializer->decoder);
            break;
        default:
            assert(false);
    }
    queuestream_close(deserializer->source);
    deserializer->callback = NULL_ACTION_1;
    async_wound(deserializer->async, deserializer);
    set_deserializer_state(deserializer, DESERIALIZER_CLOSED);
}

static void _close(void *obj)
{
    deserializer_close(obj);
}

FSTRACE_DECL(ASYNC_DESERIALIZER_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void deserializer_register_callback(deserializer_t *deserializer,
                                    action_1 action)
{
    FSTRACE(ASYNC_DESERIALIZER_REGISTER, deserializer->uid, action.obj,
            action.act);
    deserializer->callback = action;
}

static void _register_callback(void *obj, action_1 action)
{
    deserializer_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_DESERIALIZER_UNREGISTER, "UID=%64u");

void deserializer_unregister_callback(deserializer_t *deserializer)
{
    FSTRACE(ASYNC_DESERIALIZER_UNREGISTER, deserializer->uid);
    deserializer->callback = NULL_ACTION_1;
}

static void _unregister_callback(void *obj)
{
    deserializer_unregister_callback(obj);
}

static const struct yield_1_vt deserializer_vt = {
    .receive = _receive,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

yield_1 deserializer_as_yield_1(deserializer_t *deserializer)
{
    return (yield_1) { deserializer, &deserializer_vt };
}
