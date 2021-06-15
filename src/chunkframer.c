#include "chunkframer.h"

#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"
#include "chunkdecoder.h"
#include "deserializer.h"

struct chunkframer {
    async_t *async;
    uint64_t uid;
    deserializer_t *deserializer;
};

static bytestream_2 open_decoder(void *obj, bytestream_1 source)
{
    chunkframer_t *framer = obj;
    chunkdecoder_t *decoder =
        chunk_decode(framer->async, source, CHUNKDECODER_DETACH_AFTER_TRAILER);
    return chunkdecoder_as_bytestream_2(decoder);
}

FSTRACE_DECL(ASYNC_CHUNKFRAMER_CREATE, "UID=%64u PTR=%p ASYNC=%p SOURCE=%p");

chunkframer_t *open_chunkframer(async_t *async, bytestream_1 source)
{
    chunkframer_t *framer = fsalloc(sizeof *framer);
    framer->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_CHUNKFRAMER_CREATE, framer->uid, framer, async, source.obj);
    framer->async = async;
    framer->deserializer =
        open_deserializer(async, source, open_decoder, framer);
    return framer;
}

FSTRACE_DECL(ASYNC_CHUNKFRAMER_RECEIVE, "UID=%64u FRAME=%p ERRNO=%e");

bytestream_1 *chunkframer_receive(chunkframer_t *framer)
{
    bytestream_1 *frame = deserializer_receive(framer->deserializer);
    FSTRACE(ASYNC_CHUNKFRAMER_RECEIVE, framer->uid, frame);
    return frame;
}

static void *_receive(void *obj)
{
    return chunkframer_receive(obj);
}

FSTRACE_DECL(ASYNC_CHUNKFRAMER_CLOSE, "UID=%64u");

void chunkframer_close(chunkframer_t *framer)
{
    FSTRACE(ASYNC_CHUNKFRAMER_CLOSE, framer->uid);
    deserializer_close(framer->deserializer);
    async_wound(framer->async, framer);
}

static void _close(void *obj)
{
    chunkframer_close(obj);
}

FSTRACE_DECL(ASYNC_CHUNKFRAMER_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void chunkframer_register_callback(chunkframer_t *framer, action_1 action)
{
    FSTRACE(ASYNC_CHUNKFRAMER_REGISTER, framer->uid, action.obj, action.act);
    deserializer_register_callback(framer->deserializer, action);
}

static void _register_callback(void *obj, action_1 action)
{
    chunkframer_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_CHUNKFRAMER_UNREGISTER, "UID=%64u");

void chunkframer_unregister_callback(chunkframer_t *framer)
{
    FSTRACE(ASYNC_CHUNKFRAMER_UNREGISTER, framer->uid);
    chunkframer_register_callback(framer, NULL_ACTION_1);
}

static void _unregister_callback(void *obj)
{
    chunkframer_unregister_callback(obj);
}

static const struct yield_1_vt chunkframer_vt = {
    .receive = _receive,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

yield_1 chunkframer_as_yield_1(chunkframer_t *framer)
{
    return (yield_1) { framer, &chunkframer_vt };
}
