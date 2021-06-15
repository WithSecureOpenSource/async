#include "reservoir.h"

#include <assert.h>
#include <errno.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"
#include "blobstream.h"
#include "queuestream.h"

struct reservoir {
    async_t *async;
    uint64_t uid;
    size_t capacity;
    size_t amount;
    bool eof_reached;
    bytestream_1 stream;
    queuestream_t *storage;
    action_1 callback;
};

FSTRACE_DECL(ASYNC_RESERVOIR_CREATE,
             "UID=%64u PTR=%p ASYNC=%p CAPACITY=%z STREAM=%p");

reservoir_t *open_reservoir(async_t *async, size_t capacity,
                            bytestream_1 stream)
{
    reservoir_t *reservoir = fsalloc(sizeof *reservoir);
    reservoir->async = async;
    reservoir->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_RESERVOIR_CREATE, reservoir->uid, reservoir, async, capacity,
            stream.obj);
    reservoir->capacity = capacity;
    reservoir->amount = 0;
    reservoir->eof_reached = false;
    reservoir->stream = stream;
    reservoir->storage = make_queuestream(async);
    reservoir->callback = NULL_ACTION_1;
    return reservoir;
}

FSTRACE_DECL(ASYNC_RESERVOIR_CLOSE, "UID=%64u");

void reservoir_close(reservoir_t *reservoir)
{
    FSTRACE(ASYNC_RESERVOIR_CLOSE, reservoir->uid);
    assert(reservoir->async);
    queuestream_close(reservoir->storage);
    bytestream_1_close(reservoir->stream);
    async_wound(reservoir->async, reservoir);
    reservoir->async = NULL;
}

size_t reservoir_amount(reservoir_t *reservoir)
{
    return reservoir->amount;
}

FSTRACE_DECL(ASYNC_RESERVOIR_FILLING, "UID=%64u AMOUNT=%z");
FSTRACE_DECL(ASYNC_RESERVOIR_FILLED, "UID=%64u");
FSTRACE_DECL(ASYNC_RESERVOIR_OVERFLOW, "UID=%64u");
FSTRACE_DECL(ASYNC_RESERVOIR_FILL_FAIL, "UID=%64u ERRNO=%e");

bool reservoir_fill(reservoir_t *reservoir)
{
    if (reservoir->eof_reached) {
        FSTRACE(ASYNC_RESERVOIR_FILLED, reservoir->uid);
        return true;
    }
    ssize_t count;
    for (;;) {
        size_t available = reservoir->capacity - reservoir->amount;
        if (!available) {
            uint8_t extra;
            count = bytestream_1_read(reservoir->stream, &extra, 1);
            if (count <= 0)
                break;
            FSTRACE(ASYNC_RESERVOIR_OVERFLOW, reservoir->uid);
            errno = ENOSPC;
            return false;
        }
        uint8_t blob[2000];
        size_t size = sizeof blob;
        if (size > available)
            size = available;
        count = bytestream_1_read(reservoir->stream, blob, size);
        if (count <= 0)
            break;
        reservoir->amount += count;
        FSTRACE(ASYNC_RESERVOIR_FILLING, reservoir->uid, reservoir->amount);
        queuestream_enqueue_bytes(reservoir->storage, blob, count);
    }
    if (count < 0) {
        FSTRACE(ASYNC_RESERVOIR_FILL_FAIL, reservoir->uid);
        return false;
    }
    FSTRACE(ASYNC_RESERVOIR_FILLED, reservoir->uid);
    queuestream_terminate(reservoir->storage);
    queuestream_register_callback(reservoir->storage, reservoir->callback);
    reservoir->eof_reached = true;
    return true;
}

FSTRACE_DECL(ASYNC_RESERVOIR_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_RESERVOIR_READ_DUMP, "UID=%64u DATA=%B");

ssize_t reservoir_read(reservoir_t *reservoir, void *buf, size_t count)
{
    ssize_t n = queuestream_read(reservoir->storage, buf, count);
    FSTRACE(ASYNC_RESERVOIR_READ, reservoir->uid, count, n);
    FSTRACE(ASYNC_RESERVOIR_READ_DUMP, reservoir->uid, buf, n);
    return n;
}

FSTRACE_DECL(ASYNC_RESERVOIR_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void reservoir_register_callback(reservoir_t *reservoir, action_1 action)
{
    FSTRACE(ASYNC_RESERVOIR_REGISTER, reservoir->uid, action.obj, action.act);
    reservoir->callback = action;
    if (reservoir->eof_reached)
        queuestream_register_callback(reservoir->storage, action);
    else
        bytestream_1_register_callback(reservoir->stream, action);
}

FSTRACE_DECL(ASYNC_RESERVOIR_UNREGISTER, "UID=%64u");

void reservoir_unregister_callback(reservoir_t *reservoir)
{
    FSTRACE(ASYNC_RESERVOIR_UNREGISTER, reservoir->uid);
    reservoir->callback = NULL_ACTION_1;
    if (reservoir->eof_reached)
        queuestream_unregister_callback(reservoir->storage);
    else
        bytestream_1_unregister_callback(reservoir->stream);
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return reservoir_read(obj, buf, count);
}

static void _close(void *obj)
{
    reservoir_close(obj);
}

static void _register_callback(void *obj, action_1 action)
{
    reservoir_register_callback(obj, action);
}

static void _unregister_callback(void *obj)
{
    reservoir_unregister_callback(obj);
}

static const struct bytestream_1_vt reservoir_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

bytestream_1 reservoir_as_bytestream_1(reservoir_t *reservoir)
{
    return (bytestream_1) { reservoir, &reservoir_vt };
}
