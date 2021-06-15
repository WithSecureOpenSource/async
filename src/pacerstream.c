#include "pacerstream.h"

#include <assert.h>
#include <errno.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"

struct pacerstream {
    async_t *async;
    uint64_t uid;
    bytestream_1 stream;
    double byterate, quota;
    size_t min_burst, max_burst;
    uint64_t prev_t;
    action_1 callback;
    async_timer_t *retry_timer;
};

FSTRACE_DECL(ASYNC_PACERSTREAM_RETRY, "UID=%64u");

static void retry(pacerstream_t *pacer)
{
    if (pacer->async == NULL)
        return;
    FSTRACE(ASYNC_PACERSTREAM_RETRY, pacer->uid);
    pacer->retry_timer = NULL;
    action_1_perf(pacer->callback);
}

FSTRACE_DECL(ASYNC_PACERSTREAM_READ_POSTPONE, "UID=%64u WANT=%z");
FSTRACE_DECL(ASYNC_PACERSTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_PACERSTREAM_READ_DUMP, "UID=%64u DATA=%B");

ssize_t pacerstream_read(pacerstream_t *pacer, void *buf, size_t count)
{
    if (pacer->async == NULL) {
        errno = EBADF;
        return -1;
    }
    if (pacer->retry_timer != NULL)
        async_timer_cancel(pacer->async, pacer->retry_timer);
    pacer->retry_timer = NULL;
    uint64_t t = async_now(pacer->async);
    pacer->quota += (t - pacer->prev_t) * 1e-09 * pacer->byterate;
    if (pacer->quota > pacer->max_burst)
        pacer->quota = pacer->max_burst;
    pacer->prev_t = t;
    if (pacer->quota < pacer->min_burst) {
        uint64_t delay = (uint64_t)((pacer->min_burst - pacer->quota) /
                                    pacer->byterate * 1e+09);
        pacer->retry_timer =
            async_timer_start(pacer->async, t + delay,
                              (action_1) { pacer, (act_1) retry });
        FSTRACE(ASYNC_PACERSTREAM_READ_POSTPONE, pacer->uid, count);
        errno = EAGAIN;
        return -1;
    }
    if (count > pacer->quota)
        count = (size_t) pacer->quota;
    ssize_t n = bytestream_1_read(pacer->stream, buf, count);
    if (n > 0)
        pacer->quota -= n;
    FSTRACE(ASYNC_PACERSTREAM_READ, pacer->uid, count, n);
    FSTRACE(ASYNC_PACERSTREAM_READ_DUMP, pacer->uid, buf, n);
    return n;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return pacerstream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_PACERSTREAM_CLOSE, "UID=%64u");

void pacerstream_close(pacerstream_t *pacer)
{
    FSTRACE(ASYNC_PACERSTREAM_CLOSE, pacer->uid);
    assert(pacer->async != NULL);
    bytestream_1_close(pacer->stream);
    if (pacer->retry_timer != NULL)
        async_timer_cancel(pacer->async, pacer->retry_timer);
    async_wound(pacer->async, pacer);
    pacer->async = NULL;
}

static void _close(void *obj)
{
    pacerstream_close(obj);
}

FSTRACE_DECL(ASYNC_PACERSTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void pacerstream_register_callback(pacerstream_t *pacer, action_1 action)
{
    FSTRACE(ASYNC_PACERSTREAM_REGISTER, pacer->uid, action.obj, action.act);
    pacer->callback = action;
    bytestream_1_register_callback(pacer->stream, action);
}

static void _register_callback(void *obj, action_1 action)
{
    pacerstream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_PACERSTREAM_UNREGISTER, "UID=%64u");

void pacerstream_unregister_callback(pacerstream_t *pacer)
{
    FSTRACE(ASYNC_PACERSTREAM_UNREGISTER, pacer->uid);
    pacer->callback = NULL_ACTION_1;
    bytestream_1_unregister_callback(pacer->stream);
}

static void _unregister_callback(void *obj)
{
    pacerstream_unregister_callback(obj);
}

static const struct bytestream_1_vt pacerstream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 pacerstream_as_bytestream_1(pacerstream_t *pacer)
{
    return (bytestream_1) { pacer, &pacerstream_vt };
}

FSTRACE_DECL(ASYNC_PACESTREAM_CREATE,
             "UID=%64u PTR=%p ASYNC=%p STREAM=%p RATE=%f MIN=%z MAX=%z");

pacerstream_t *pace_stream(async_t *async, bytestream_1 stream, double byterate,
                           size_t min_burst, size_t max_burst)
{
    pacerstream_t *pacer = fsalloc(sizeof *pacer);
    pacer->async = async;
    pacer->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_PACESTREAM_CREATE, pacer->uid, pacer, async, stream.obj,
            byterate, min_burst, max_burst);
    pacer->stream = stream;
    pacer->byterate = byterate;
    if (min_burst < 1)
        pacer->min_burst = 1;
    else
        pacer->min_burst = min_burst;
    pacer->max_burst = max_burst;
    pacer->callback = NULL_ACTION_1;
    pacer->retry_timer = NULL;
    pacer->quota = 0;
    pacer->prev_t = async_now(pacer->async);
    return pacer;
}

FSTRACE_DECL(ASYNC_PACESTREAM_RESET, "UID=%64u");

void pacerstream_reset(pacerstream_t *pacer)
{
    FSTRACE(ASYNC_PACESTREAM_RESET, pacer->uid);
    pacer->quota = 0;
    pacer->prev_t = async_now(pacer->async);
}
