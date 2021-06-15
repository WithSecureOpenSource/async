#include "farewellstream.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"

struct farewellstream {
    async_t *async;
    uint64_t uid;
    bytestream_1 stream;
    action_1 farewell_action;
    bool sync;
};

FSTRACE_DECL(ASYNC_FAREWELLSTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_FAREWELLSTREAM_READ_DUMP, "UID=%64u DATA=%B");

ssize_t farewellstream_read(farewellstream_t *fwstr, void *buf, size_t count)
{
    ssize_t n = bytestream_1_read(fwstr->stream, buf, count);
    FSTRACE(ASYNC_FAREWELLSTREAM_READ, fwstr->uid, count, n);
    FSTRACE(ASYNC_FAREWELLSTREAM_READ_DUMP, fwstr->uid, buf, n);
    return n;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return farewellstream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_FAREWELLSTREAM_CLOSE, "UID=%64u");

void farewellstream_close(farewellstream_t *fwstr)
{
    FSTRACE(ASYNC_FAREWELLSTREAM_CLOSE, fwstr->uid);
    assert(fwstr->async != NULL);
    bytestream_1_close(fwstr->stream);
    async_wound(fwstr->async, fwstr);
    async_t *async = fwstr->async;
    fwstr->async = NULL;
    if (fwstr->sync)
        action_1_perf(fwstr->farewell_action);
    else
        async_execute(async, fwstr->farewell_action);
}

static void _close(void *obj)
{
    farewellstream_close(obj);
}

FSTRACE_DECL(ASYNC_FAREWELLSTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void farewellstream_register_callback(farewellstream_t *fwstr, action_1 action)
{
    FSTRACE(ASYNC_FAREWELLSTREAM_REGISTER, fwstr->uid, action.obj, action.act);
    bytestream_1_register_callback(fwstr->stream, action);
}

static void _register_callback(void *obj, action_1 action)
{
    farewellstream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_FAREWELLSTREAM_UNREGISTER, "UID=%64u");

void farewellstream_unregister_callback(farewellstream_t *fwstr)
{
    FSTRACE(ASYNC_FAREWELLSTREAM_UNREGISTER, fwstr->uid);
    bytestream_1_unregister_callback(fwstr->stream);
}

static void _unregister_callback(void *obj)
{
    farewellstream_unregister_callback(obj);
}

static const struct bytestream_1_vt farewellstream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 farewellstream_as_bytestream_1(farewellstream_t *fwstr)
{
    return (bytestream_1) { fwstr, &farewellstream_vt };
}

FSTRACE_DECL(ASYNC_FAREWELLSTREAM_CREATE,
             "UID=%64u PTR=%p ASYNC=%p STREAM=%p OBJ=%p ACT=%p MODE=%s");

static farewellstream_t *make_farewellstream(async_t *async,
                                             bytestream_1 stream,
                                             action_1 farewell_action,
                                             bool sync)
{
    farewellstream_t *fwstr = fsalloc(sizeof *fwstr);
    fwstr->async = async;
    fwstr->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_FAREWELLSTREAM_CREATE, fwstr->uid, fwstr, async, stream.obj,
            farewell_action.obj, farewell_action.act,
            sync ? "SYNC" : "RELAXED");
    fwstr->stream = stream;
    fwstr->farewell_action = farewell_action;
    fwstr->sync = sync;
    return fwstr;
}

farewellstream_t *open_farewellstream(async_t *async, bytestream_1 stream,
                                      action_1 farewell_action)
{
    return make_farewellstream(async, stream, farewell_action, true);
}

farewellstream_t *open_relaxed_farewellstream(async_t *async,
                                              bytestream_1 stream,
                                              action_1 farewell_action)
{
    return make_farewellstream(async, stream, farewell_action, false);
}
