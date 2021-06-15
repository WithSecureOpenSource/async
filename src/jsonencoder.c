#include "jsonencoder.h"

#include <assert.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"
#include "blobstream.h"

struct jsonencoder {
    async_t *async;
    uint64_t uid;
    size_t size;
    blobstream_t *blobstr;
};

FSTRACE_DECL(ASYNC_JSONENCODER_CREATE, "UID=%64u PTR=%p ASYNC=%p SIZE=%z");
FSTRACE_DECL(ASYNC_JSONENCODER_CREATE_DUMP, "UID=%64u TEXT=%s");

jsonencoder_t *json_encode(async_t *async, json_thing_t *thing)
{
    jsonencoder_t *encoder = fsalloc(sizeof *encoder);
    encoder->async = async;
    encoder->uid = fstrace_get_unique_id();
    size_t size = encoder->size = json_utf8_encode(thing, NULL, 0);
    FSTRACE(ASYNC_JSONENCODER_CREATE, encoder->uid, encoder, async, size);
    uint8_t *blob = fsalloc(size + 1);
    json_utf8_encode(thing, blob, size + 1);
    FSTRACE(ASYNC_JSONENCODER_CREATE_DUMP, encoder->uid, blob);
    action_1 freer = { blob, (act_1) fsfree };
    encoder->blobstr = adopt_blobstream(async, blob, size, freer);
    return encoder;
}

size_t jsonencoder_size(jsonencoder_t *encoder)
{
    return encoder->size;
}

FSTRACE_DECL(ASYNC_JSONENCODER_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_JSONENCODER_READ_DUMP, "UID=%64u TEXT=%A");

ssize_t jsonencoder_read(jsonencoder_t *encoder, void *buf, size_t count)
{
    ssize_t n = blobstream_read(encoder->blobstr, buf, count);
    FSTRACE(ASYNC_JSONENCODER_READ, encoder->uid, count, n);
    FSTRACE(ASYNC_JSONENCODER_READ_DUMP, encoder->uid, buf, n);
    return n;
}

FSTRACE_DECL(ASYNC_JSONENCODER_CLOSE, "UID=%64u");

void jsonencoder_close(jsonencoder_t *encoder)
{
    FSTRACE(ASYNC_JSONENCODER_CLOSE, encoder->uid);
    assert(encoder->async);
    blobstream_close(encoder->blobstr);
    async_wound(encoder->async, encoder);
    encoder->async = NULL;
}

FSTRACE_DECL(ASYNC_JSONENCODER_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void jsonencoder_register_callback(jsonencoder_t *encoder, action_1 action)
{
    FSTRACE(ASYNC_JSONENCODER_REGISTER, encoder->uid, action.obj, action.act);
    blobstream_register_callback(encoder->blobstr, action);
}

FSTRACE_DECL(ASYNC_JSONENCODER_UNREGISTER, "UID=%64u");

void jsonencoder_unregister_callback(jsonencoder_t *encoder)
{
    FSTRACE(ASYNC_JSONENCODER_UNREGISTER, encoder->uid);
    blobstream_unregister_callback(encoder->blobstr);
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return jsonencoder_read(obj, buf, count);
}

static void _close(void *obj)
{
    jsonencoder_close(obj);
}

static void _register_callback(void *obj, action_1 action)
{
    jsonencoder_register_callback(obj, action);
}

static void _unregister_callback(void *obj)
{
    jsonencoder_unregister_callback(obj);
}

static struct bytestream_1_vt jsonencoder_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

bytestream_1 jsonencoder_as_bytestream_1(jsonencoder_t *encoder)
{
    return (bytestream_1) { encoder, &jsonencoder_vt };
}
