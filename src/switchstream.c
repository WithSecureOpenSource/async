#include "switchstream.h"

#include <assert.h>
#include <errno.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"

struct switchstream {
    async_t *async;
    uint64_t uid;
    bytestream_1 stream;
    action_1 callback;
};

FSTRACE_DECL(ASYNC_SWITCHSTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_SWITCHSTREAM_READ_DUMP, "UID=%64u DATA=%B");

ssize_t switchstream_read(switchstream_t *swstr, void *buf, size_t count)
{
    ssize_t n = bytestream_1_read(swstr->stream, buf, count);
    FSTRACE(ASYNC_SWITCHSTREAM_READ, swstr->uid, count, n);
    FSTRACE(ASYNC_SWITCHSTREAM_READ_DUMP, swstr->uid, buf, n);
    return n;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return switchstream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_SWITCHSTREAM_CLOSE, "UID=%64u");

void switchstream_close(switchstream_t *swstr)
{
    FSTRACE(ASYNC_SWITCHSTREAM_CLOSE, swstr->uid);
    assert(swstr->async != NULL);
    bytestream_1_close(swstr->stream);
    async_wound(swstr->async, swstr);
    swstr->async = NULL;
}

static void _close(void *obj)
{
    switchstream_close(obj);
}

FSTRACE_DECL(ASYNC_SWITCHSTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void switchstream_register_callback(switchstream_t *swstr, action_1 action)
{
    FSTRACE(ASYNC_SWITCHSTREAM_REGISTER, swstr->uid, action.obj, action.act);
    swstr->callback = action;
    bytestream_1_register_callback(swstr->stream, action);
}

static void _register_callback(void *obj, action_1 action)
{
    switchstream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_SWITCHSTREAM_UNREGISTER, "UID=%64u");

void switchstream_unregister_callback(switchstream_t *swstr)
{
    FSTRACE(ASYNC_SWITCHSTREAM_UNREGISTER, swstr->uid);
    swstr->callback = NULL_ACTION_1;
    bytestream_1_unregister_callback(swstr->stream);
}

static void _unregister_callback(void *obj)
{
    switchstream_unregister_callback(obj);
}

static const struct bytestream_1_vt switchstream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 switchstream_as_bytestream_1(switchstream_t *swstr)
{
    return (bytestream_1) { swstr, &switchstream_vt };
}

FSTRACE_DECL(ASYNC_SWITCHSTREAM_CREATE, "UID=%64u PTR=%p ASYNC=%p STREAM=%p");

switchstream_t *open_switch_stream(async_t *async, bytestream_1 stream)
{
    switchstream_t *swstr = fsalloc(sizeof *swstr);
    swstr->async = async;
    swstr->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_SWITCHSTREAM_CREATE, swstr->uid, swstr, async, stream.obj);
    swstr->stream = stream;
    swstr->callback = NULL_ACTION_1;
    return swstr;
}

FSTRACE_DECL(ASYNC_SWITCHSTREAM_REATTACH, "UID=%64u OLD=%p NEW=%p");

bytestream_1 switchstream_reattach(switchstream_t *swstr, bytestream_1 stream)
{
    bytestream_1 old = swstr->stream;
    FSTRACE(ASYNC_SWITCHSTREAM_REATTACH, swstr->uid, old.obj, stream.obj);
    swstr->stream = stream;
    bytestream_1_register_callback(stream, swstr->callback);
    async_execute(swstr->async, swstr->callback);
    return old;
}
