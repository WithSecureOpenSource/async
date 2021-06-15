#include "naiveframer.h"

#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"
#include "deserializer.h"
#include "naivedecoder.h"

struct naiveframer {
    async_t *async;
    uint64_t uid;
    deserializer_t *deserializer;
    uint8_t terminator, escape;
};

static bytestream_2 open_decoder(void *obj, bytestream_1 source)
{
    naiveframer_t *framer = obj;
    naivedecoder_t *decoder =
        naive_decode(framer->async, source, NAIVEDECODER_DETACH,
                     framer->terminator, framer->escape);
    return naivedecoder_as_bytestream_2(decoder);
}

FSTRACE_DECL(ASYNC_NAIVEFRAMER_CREATE, "UID=%64u PTR=%p ASYNC=%p SOURCE=%p");

naiveframer_t *open_naiveframer(async_t *async, bytestream_1 source,
                                uint8_t terminator, uint8_t escape)
{
    naiveframer_t *framer = fsalloc(sizeof *framer);
    framer->async = async;
    framer->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_NAIVEFRAMER_CREATE, framer->uid, framer, async, source.obj);
    framer->deserializer =
        open_deserializer(async, source, open_decoder, framer);
    framer->terminator = terminator;
    framer->escape = escape;
    return framer;
}

FSTRACE_DECL(ASYNC_NAIVEFRAMER_RECEIVE, "UID=%64u FRAME=%p ERRNO=%e");

bytestream_1 *naiveframer_receive(naiveframer_t *framer)
{
    bytestream_1 *frame = deserializer_receive(framer->deserializer);
    FSTRACE(ASYNC_NAIVEFRAMER_RECEIVE, framer->uid, frame);
    return frame;
}

static void *_receive(void *obj)
{
    return naiveframer_receive(obj);
}

FSTRACE_DECL(ASYNC_NAIVEFRAMER_CLOSE, "UID=%64u");

void naiveframer_close(naiveframer_t *framer)
{
    FSTRACE(ASYNC_NAIVEFRAMER_CLOSE, framer->uid);
    deserializer_close(framer->deserializer);
    async_wound(framer->async, framer);
}

static void _close(void *obj)
{
    naiveframer_close(obj);
}

FSTRACE_DECL(ASYNC_NAIVEFRAMER_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void naiveframer_register_callback(naiveframer_t *framer, action_1 action)
{
    FSTRACE(ASYNC_NAIVEFRAMER_REGISTER, framer->uid, action.obj, action.act);
    deserializer_register_callback(framer->deserializer, action);
}

static void _register_callback(void *obj, action_1 action)
{
    naiveframer_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_NAIVEFRAMER_UNREGISTER, "UID=%64u");

void naiveframer_unregister_callback(naiveframer_t *framer)
{
    FSTRACE(ASYNC_NAIVEFRAMER_UNREGISTER, framer->uid);
    naiveframer_register_callback(framer, NULL_ACTION_1);
}

static void _unregister_callback(void *obj)
{
    naiveframer_unregister_callback(obj);
}

static const struct yield_1_vt naiveframer_vt = {
    .receive = _receive,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

yield_1 naiveframer_as_yield_1(naiveframer_t *framer)
{
    return (yield_1) { framer, &naiveframer_vt };
}
