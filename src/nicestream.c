#include "nicestream.h"

#include <assert.h>
#include <errno.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"

struct nicestream {
    async_t *async;
    uint64_t uid;
    bytestream_1 stream;
    size_t this_burst, max_burst;
    action_1 callback;
};

FSTRACE_DECL(ASYNC_NICESTREAM_RETRY, "UID=%64u");

static void retry(nicestream_t *nice)
{
    FSTRACE(ASYNC_NICESTREAM_RETRY, nice->uid);
    if (nice->async == NULL)
        return;
    action_1_perf(nice->callback);
}

FSTRACE_DECL(ASYNC_NICESTREAM_BACK_OFF, "UID=%64u WANT=%z");
FSTRACE_DECL(ASYNC_NICESTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_NICESTREAM_READ_DUMP, "UID=%64u DATA=%B");

ssize_t nicestream_read(nicestream_t *nice, void *buf, size_t count)
{
    if (nice->this_burst > nice->max_burst) {
        FSTRACE(ASYNC_NICESTREAM_BACK_OFF, nice->uid, count);
        nice->this_burst = 0;
        async_execute(nice->async, (action_1) { nice, (act_1) retry });
        errno = EAGAIN;
        return -1;
    }
    ssize_t n = bytestream_1_read(nice->stream, buf, count);
    if (n < 0)
        nice->this_burst = 0;
    else
        nice->this_burst += n;
    FSTRACE(ASYNC_NICESTREAM_READ, nice->uid, count, n);
    FSTRACE(ASYNC_NICESTREAM_READ_DUMP, nice->uid, buf, n);
    return n;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return nicestream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_NICESTREAM_CLOSE, "UID=%64u");

void nicestream_close(nicestream_t *nice)
{
    FSTRACE(ASYNC_NICESTREAM_CLOSE, nice->uid);
    assert(nice->async != NULL);
    bytestream_1_close(nice->stream);
    async_wound(nice->async, nice);
    nice->async = NULL;
}

static void _close(void *obj)
{
    nicestream_close(obj);
}

FSTRACE_DECL(ASYNC_NICESTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void nicestream_register_callback(nicestream_t *nice, action_1 action)
{
    FSTRACE(ASYNC_NICESTREAM_REGISTER, nice->uid, action.obj, action.act);
    nice->callback = action;
    bytestream_1_register_callback(nice->stream, action);
}

static void _register_callback(void *obj, action_1 action)
{
    nicestream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_NICESTREAM_UNREGISTER, "UID=%64u");

void nicestream_unregister_callback(nicestream_t *nice)
{
    FSTRACE(ASYNC_NICESTREAM_UNREGISTER, nice->uid);
    nice->callback = NULL_ACTION_1;
    bytestream_1_unregister_callback(nice->stream);
}

static void _unregister_callback(void *obj)
{
    nicestream_unregister_callback(obj);
}

static const struct bytestream_1_vt nicestream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 nicestream_as_bytestream_1(nicestream_t *nice)
{
    return (bytestream_1) { nice, &nicestream_vt };
}

FSTRACE_DECL(ASYNC_NICESTREAM_CREATE,
             "UID=%64u PTR=%p ASYNC=%p STREAM=%p MAX-BURST=%z");

nicestream_t *make_nice(async_t *async, bytestream_1 stream, size_t max_burst)
{
    nicestream_t *nice = fsalloc(sizeof *nice);
    nice->async = async;
    nice->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_NICESTREAM_CREATE, nice->uid, nice, async, stream.obj,
            max_burst);
    nice->stream = stream;
    nice->this_burst = 0;
    nice->max_burst = max_burst;
    nice->callback = NULL_ACTION_1;
    return nice;
}
