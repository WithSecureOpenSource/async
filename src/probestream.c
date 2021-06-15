#include "probestream.h"

#include <assert.h>
#include <errno.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"
#include "bytestream_1.h"

struct probestream {
    async_t *async;
    uint64_t uid;
    bytestream_1 source;
    void *obj;
    ssize_t read_count;
    probestream_close_probe_cb_t close_action;
    probestream_read_probe_cb_t read_action;
};

FSTRACE_DECL(ASYNC_PROBESTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void probestream_register_callback(probestream_t *probestr, action_1 action)
{
    FSTRACE(ASYNC_PROBESTREAM_REGISTER, probestr->uid, action.obj, action.act);
    bytestream_1_register_callback(probestr->source, action);
}

static void _register_callback(void *obj, action_1 action)
{
    probestream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_PROBESTREAM_UNREGISTER, "UID=%64u");

void probestream_unregister_callback(probestream_t *probestr)
{
    FSTRACE(ASYNC_PROBESTREAM_UNREGISTER, probestr->uid);
    bytestream_1_unregister_callback(probestr->source);
}

static void _unregister_callback(void *obj)
{
    probestream_unregister_callback(obj);
}

FSTRACE_DECL(ASYNC_PROBESTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_PROBESTREAM_READ_DUMP, "UID=%64u DATA=%B");

ssize_t probestream_read(probestream_t *probestr, void *buf, size_t count)
{
    ssize_t read_result = bytestream_1_read(probestr->source, buf, count);
    int _errno = errno;
    probestr->read_action(probestr->obj, buf, count, read_result);
    errno = _errno;
    FSTRACE(ASYNC_PROBESTREAM_READ, probestr->uid, count, read_result);
    FSTRACE(ASYNC_PROBESTREAM_READ_DUMP, probestr->uid, buf, read_result);
    return read_result;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return probestream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_PROBESTREAM_CLOSE, "UID=%64u");

void probestream_close(probestream_t *probestr)
{
    FSTRACE(ASYNC_PROBESTREAM_CLOSE, probestr->uid);
    assert(probestr->async != NULL);
    bytestream_1_close(probestr->source);
    async_wound(probestr->async, probestr);
    probestr->close_action(probestr->obj);
    probestr->async = NULL;
}

static void _close(void *obj)
{
    probestream_close(obj);
}

static const struct bytestream_1_vt probestream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 probestream_as_bytestream_1(probestream_t *probestr)
{
    return (bytestream_1) { probestr, &probestream_vt };
}

FSTRACE_DECL(ASYNC_PROBESTREAM_CREATE,
             "UID=%64u PTR=%p ASYNC=%p OBJ=%p SOURCE=%p "
             "CLOSE-CB=%p READ-CB=%p");

probestream_t *open_probestream(async_t *async, void *obj, bytestream_1 source,
                                probestream_close_probe_cb_t close_cb,
                                probestream_read_probe_cb_t read_cb)
{
    probestream_t *probestr = fsalloc(sizeof *probestr);
    probestr->async = async;
    probestr->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_PROBESTREAM_CREATE, probestr->uid, probestr, async, obj,
            source.obj, close_cb, read_cb);
    probestr->source = source;
    probestr->obj = obj;
    probestr->close_action = close_cb;
    probestr->read_action = read_cb;
    return probestr;
}
