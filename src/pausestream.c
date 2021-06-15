#include "pausestream.h"

#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"

struct pausestream {
    async_t *async;
    uint64_t uid;
    int fd;
    off_t bytes_read;
    pausestream_limit_cb_1 limit_cb;
    void *get_limit_ctx;
};

FSTRACE_DECL(ASYNC_PAUSESTREAM_READ_IN_LIMBO, "UID=%64u WANT=%z");
FSTRACE_DECL(ASYNC_PAUSESTREAM_READ_PAUSED,
             "UID=%64u WANT=%z LIMIT=%64u CURSOR=%64u");
FSTRACE_DECL(ASYNC_PAUSESTREAM_READ,
             "UID=%64u WANT=%z GOT=%z CURSOR=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_PAUSESTREAM_READ_DUMP, "UID=%64u DATA=%B");

ssize_t pausestream_read(pausestream_t *pausestr, void *buf, size_t count)
{
    ssize_t result;
    off_t limit;
    if (pausestr->limit_cb.limit == NULL) {
        FSTRACE(ASYNC_PAUSESTREAM_READ_IN_LIMBO, pausestr->uid, count);
        errno = EAGAIN;
        return -1;
    }
    limit = pausestr->limit_cb.limit(pausestr->limit_cb.obj);
    if (limit >= 0) {
        if (pausestr->bytes_read >= limit) {
            FSTRACE(ASYNC_PAUSESTREAM_READ_PAUSED, pausestr->uid, count,
                    (uint64_t) limit, (uint64_t) pausestr->bytes_read);
            errno = EAGAIN;
            return -1;
        }
        if (count > limit - pausestr->bytes_read) {
            count = limit - pausestr->bytes_read;
        }
    }
    result = read(pausestr->fd, buf, count);
    if (result > 0)
        pausestr->bytes_read += result;
    FSTRACE(ASYNC_PAUSESTREAM_READ, pausestr->uid, count, result,
            (uint64_t) pausestr->bytes_read);
    FSTRACE(ASYNC_PAUSESTREAM_READ_DUMP, pausestr->uid, buf, result);
    return result;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return pausestream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_PAUSESTREAM_CLOSE, "UID=%64u");

void pausestream_close(pausestream_t *pausestr)
{
    FSTRACE(ASYNC_PAUSESTREAM_CLOSE, pausestr->uid);
    close(pausestr->fd);
    assert(pausestr->async != NULL);
    async_wound(pausestr->async, pausestr);
    pausestr->async = NULL;
}

static void _close(void *obj)
{
    pausestream_close(obj);
}

FSTRACE_DECL(ASYNC_PAUSESTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void pausestream_register_callback(pausestream_t *pausestr, action_1 action)
{
    FSTRACE(ASYNC_PAUSESTREAM_REGISTER, pausestr->uid, action.obj, action.act);
}

static void _register_callback(void *obj, action_1 action)
{
    pausestream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_PAUSESTREAM_UNREGISTER, "UID=%64u");

void pausestream_unregister_callback(pausestream_t *pausestr)
{
    FSTRACE(ASYNC_PAUSESTREAM_UNREGISTER, pausestr->uid);
}

static void _unregister_callback(void *obj)
{
    pausestream_unregister_callback(obj);
}

static const struct bytestream_1_vt pausestream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 pausestream_as_bytestream_1(pausestream_t *encoder)
{
    return (bytestream_1) { encoder, &pausestream_vt };
}

void pausestream_set_limit_callback(pausestream_t *pausestr,
                                    pausestream_limit_cb_1 limit_cb)
{
    pausestr->limit_cb = limit_cb;
}

FSTRACE_DECL(ASYNC_PAUSESTREAM_CREATE, "UID=%64u PTR=%p ASYNC=%p FD=%d");

pausestream_t *open_pausestream(async_t *async, int fd)
{
    pausestream_t *pausestr = fsalloc(sizeof *pausestr);
    pausestr->async = async;
    pausestr->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_PAUSESTREAM_CREATE, pausestr->uid, pausestr, async, fd);
    pausestr->fd = fd;
    pausestr->bytes_read = 0;
    pausestr->limit_cb = (pausestream_limit_cb_1) { NULL, NULL };
    return pausestr;
}
