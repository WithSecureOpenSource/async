#include "tricklestream.h"

#include <assert.h>
#include <errno.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"

struct tricklestream {
    async_t *async;
    uint64_t uid;
    bytestream_1 stream;
    uint64_t due, interval;
    action_1 callback;
    async_timer_t *retry_timer;
};

FSTRACE_DECL(ASYNC_TRICKLESTREAM_RETRY, "UID=%64u");

static void retry(tricklestream_t *trickle)
{
    if (trickle->async == NULL)
        return;
    FSTRACE(ASYNC_TRICKLESTREAM_RETRY, trickle->uid);
    trickle->retry_timer = NULL;
    action_1_perf(trickle->callback);
}

static ssize_t do_read(tricklestream_t *trickle, void *buf, size_t count)
{
    if (trickle->interval == 0)
        return bytestream_1_read(trickle->stream, buf, count);
    if (count == 0)
        return 0;
    if (trickle->retry_timer != NULL)
        async_timer_cancel(trickle->async, trickle->retry_timer);
    trickle->retry_timer = NULL;
    uint64_t now = async_now(trickle->async);
    if (now < trickle->due) {
        trickle->retry_timer =
            async_timer_start(trickle->async, trickle->due,
                              (action_1) { trickle, (act_1) retry });
        errno = EAGAIN;
        return -1;
    }
    ssize_t n = bytestream_1_read(trickle->stream, buf, 1);
    if (n > 0)
        trickle->due = now + trickle->interval; /* no point catching up */
    return n;
}

FSTRACE_DECL(ASYNC_TRICKLESTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_TRICKLESTREAM_READ_DUMP, "UID=%64u DATA=%B");

ssize_t tricklestream_read(tricklestream_t *trickle, void *buf, size_t count)
{
    ssize_t n = do_read(trickle, buf, count);
    FSTRACE(ASYNC_TRICKLESTREAM_READ, trickle->uid, count, n);
    FSTRACE(ASYNC_TRICKLESTREAM_READ_DUMP, trickle->uid, buf, n);
    return n;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return tricklestream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_TRICKLESTREAM_CLOSE, "UID=%64u");

void tricklestream_close(tricklestream_t *trickle)
{
    FSTRACE(ASYNC_TRICKLESTREAM_CLOSE, trickle->uid);
    assert(trickle->async != NULL);
    bytestream_1_close(trickle->stream);
    if (trickle->retry_timer != NULL)
        async_timer_cancel(trickle->async, trickle->retry_timer);
    async_wound(trickle->async, trickle);
    trickle->async = NULL;
}

static void _close(void *obj)
{
    tricklestream_close(obj);
}

FSTRACE_DECL(ASYNC_TRICKLESTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void tricklestream_register_callback(tricklestream_t *trickle, action_1 action)
{
    FSTRACE(ASYNC_TRICKLESTREAM_REGISTER, trickle->uid, action.obj, action.act);
    trickle->callback = action;
    bytestream_1_register_callback(trickle->stream, action);
}

static void _register_callback(void *obj, action_1 action)
{
    tricklestream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_TRICKLESTREAM_UNREGISTER, "UID=%64u");

void tricklestream_unregister_callback(tricklestream_t *trickle)
{
    FSTRACE(ASYNC_TRICKLESTREAM_UNREGISTER, trickle->uid);
    trickle->callback = NULL_ACTION_1;
    bytestream_1_unregister_callback(trickle->stream);
}

static void _unregister_callback(void *obj)
{
    tricklestream_unregister_callback(obj);
}

static const struct bytestream_1_vt tricklestream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 tricklestream_as_bytestream_1(tricklestream_t *trickle)
{
    return (bytestream_1) { trickle, &tricklestream_vt };
}

FSTRACE_DECL(ASYNC_TRICKLESTREAM_CREATE,
             "UID=%64u PTR=%p ASYNC=%p STREAM=%p INTERVAL=%f");

tricklestream_t *open_tricklestream(async_t *async, bytestream_1 stream,
                                    double interval)
{
    tricklestream_t *trickle = fsalloc(sizeof *trickle);
    trickle->async = async;
    trickle->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_TRICKLESTREAM_CREATE, trickle->uid, trickle, async,
            stream.obj, interval);
    trickle->stream = stream;
    trickle->interval = (uint64_t)(ASYNC_S * interval);
    trickle->due = async_now(async) + trickle->interval;
    trickle->callback = NULL_ACTION_1;
    trickle->retry_timer = NULL;
    return trickle;
}

FSTRACE_DECL(ASYNC_TRICKLESTREAM_RELEASE, "UID=%64u");

void tricklestream_release(tricklestream_t *trickle)
{
    FSTRACE(ASYNC_TRICKLESTREAM_RELEASE, trickle->uid);
    trickle->interval = 0;
}
