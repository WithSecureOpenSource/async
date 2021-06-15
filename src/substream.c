#include "substream.h"

#include <assert.h>
#include <errno.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"

enum {
    SUBSTREAM_CLOSED = -1,
};

struct substream {
    async_t *async;
    uint64_t uid;
    bytestream_1 stream;
    int mode;
    size_t begin, end, counter;
};

static ssize_t do_read(substream_t *substr, void *buf, size_t count)
{
    if (substr->async == NULL) {
        errno = EBADF;
        return -1;
    }
    if (substr->mode == SUBSTREAM_CLOSED)
        return 0;
    while (substr->counter < substr->begin) {
        ssize_t skip = substr->begin - substr->counter;
        if (skip > count)
            skip = count;
        ssize_t n = bytestream_1_read(substr->stream, buf, skip);
        if (n <= 0)
            return n;
        substr->counter += n;
    }
    if (substr->mode == SUBSTREAM_NO_END)
        /* no need to maintain substr->counter here */
        return bytestream_1_read(substr->stream, buf, count);
    if (substr->counter < substr->end) {
        ssize_t include = substr->end - substr->counter;
        if (include > count)
            include = count;
        ssize_t n = bytestream_1_read(substr->stream, buf, include);
        if (n > 0)
            substr->counter += n;
        return n;
    }
    switch (substr->mode) {
        case SUBSTREAM_DETACHED:
            return 0;
        case SUBSTREAM_CLOSE_AT_END:
            bytestream_1_close(substr->stream);
            substr->mode = SUBSTREAM_CLOSED;
            return 0;
        case SUBSTREAM_FAST_FORWARD:
            for (;;) {
                ssize_t n = bytestream_1_read(substr->stream, buf, count);
                if (n <= 0)
                    return n;
            }
        default:
            abort();
    }
}

FSTRACE_DECL(ASYNC_SUBSTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_SUBSTREAM_READ_DUMP, "UID=%64u DATA=%B");

ssize_t substream_read(substream_t *substr, void *buf, size_t count)
{
    ssize_t n = do_read(substr, buf, count);
    FSTRACE(ASYNC_SUBSTREAM_READ, substr->uid, count, n);
    FSTRACE(ASYNC_SUBSTREAM_READ_DUMP, substr->uid, buf, n);
    return n;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return substream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_SUBSTREAM_CLOSE, "UID=%64u");

void substream_close(substream_t *substr)
{
    FSTRACE(ASYNC_SUBSTREAM_CLOSE, substr->uid);
    assert(substr->async != NULL);
    switch (substr->mode) {
        case SUBSTREAM_CLOSED:
        case SUBSTREAM_DETACHED:
            break;
        default:
            bytestream_1_close(substr->stream);
    }
    async_wound(substr->async, substr);
    substr->async = NULL;
}

static void _close(void *obj)
{
    substream_close(obj);
}

FSTRACE_DECL(ASYNC_SUBSTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void substream_register_callback(substream_t *substr, action_1 action)
{
    FSTRACE(ASYNC_SUBSTREAM_REGISTER, substr->uid, action.obj, action.act);
    bytestream_1_register_callback(substr->stream, action);
}

static void _register_callback(void *obj, action_1 action)
{
    substream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_SUBSTREAM_UNREGISTER, "UID=%64u");

void substream_unregister_callback(substream_t *substr)
{
    FSTRACE(ASYNC_SUBSTREAM_UNREGISTER, substr->uid);
    bytestream_1_unregister_callback(substr->stream);
}

static void _unregister_callback(void *obj)
{
    substream_unregister_callback(obj);
}

static const struct bytestream_1_vt substream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

bytestream_1 substream_as_bytestream_1(substream_t *substr)
{
    return (bytestream_1) { substr, &substream_vt };
}

static const char *trace_mode(void *pmode)
{
    switch (*(int *) pmode) {
        case SUBSTREAM_NO_END:
            return "SUBSTREAM_NO_END";
        case SUBSTREAM_FAST_FORWARD:
            return "SUBSTREAM_FAST_FORWARD";
        case SUBSTREAM_CLOSE_AT_END:
            return "SUBSTREAM_CLOSE_AT_END";
        case SUBSTREAM_DETACHED:
            return "SUBSTREAM_DETACHED";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNC_SUBSTREAM_CREATE,
             "UID=%64u PTR=%p ASYNC=%p STREAM=%p MODE=%I BEGIN=%z END=%z");

substream_t *make_substream(async_t *async, bytestream_1 stream, int mode,
                            size_t begin, size_t end)
{
    substream_t *substr = fsalloc(sizeof *substr);
    substr->async = async;
    substr->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_SUBSTREAM_CREATE, substr->uid, substr, async, stream.obj,
            trace_mode, &mode, begin, end);
    substr->stream = stream;
    substr->mode = mode;
    substr->begin = begin;
    substr->end = end;
    substr->counter = 0;
    return substr;
}
