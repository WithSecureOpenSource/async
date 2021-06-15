#include "clobberstream.h"

#include <assert.h>
#include <errno.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"

struct clobberstream {
    async_t *async;
    uint64_t uid;
    bytestream_1 stream;
    size_t cursor, offset;
    uint8_t mask[sizeof(uint64_t)];
};

static ssize_t do_read(clobberstream_t *clstr, void *buf, size_t count)
{
    ssize_t n = bytestream_1_read(clstr->stream, buf, count);
    if (n <= 0)
        return n;
    ssize_t end = clstr->cursor + n;
    ssize_t low = clstr->offset;
    ssize_t high = clstr->offset + sizeof clstr->mask;
    if (low < clstr->cursor)
        low = clstr->cursor;
    if (high > end)
        high = end;
    ssize_t i;
    for (i = low; i < high; i++)
        ((uint8_t *) buf)[i - clstr->cursor] ^= clstr->mask[i - clstr->offset];
    clstr->cursor = end;
    return n;
}

FSTRACE_DECL(ASYNC_CLOBBERSTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_CLOBBERSTREAM_READ_DUMP, "UID=%64u DATA=%B");

ssize_t clobberstream_read(clobberstream_t *clstr, void *buf, size_t count)
{
    ssize_t n = do_read(clstr, buf, count);
    FSTRACE(ASYNC_CLOBBERSTREAM_READ, clstr->uid, count, n);
    FSTRACE(ASYNC_CLOBBERSTREAM_READ_DUMP, clstr->uid, buf, n);
    return n;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return clobberstream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_CLOBBERSTREAM_CLOSE, "UID=%64u");

void clobberstream_close(clobberstream_t *clstr)
{
    FSTRACE(ASYNC_CLOBBERSTREAM_CLOSE, clstr->uid);
    assert(clstr->async != NULL);
    bytestream_1_close(clstr->stream);
    async_wound(clstr->async, clstr);
    clstr->async = NULL;
}

static void _close(void *obj)
{
    clobberstream_close(obj);
}

FSTRACE_DECL(ASYNC_CLOBBERSTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void clobberstream_register_callback(clobberstream_t *clstr, action_1 action)
{
    FSTRACE(ASYNC_CLOBBERSTREAM_REGISTER, clstr->uid, action.obj, action.act);
    bytestream_1_register_callback(clstr->stream, action);
}

static void _register_callback(void *obj, action_1 action)
{
    clobberstream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_CLOBBERSTREAM_UNREGISTER, "UID=%64u");

void clobberstream_unregister_callback(clobberstream_t *clstr)
{
    FSTRACE(ASYNC_CLOBBERSTREAM_UNREGISTER, clstr->uid);
    bytestream_1_unregister_callback(clstr->stream);
}

static void _unregister_callback(void *obj)
{
    clobberstream_unregister_callback(obj);
}

static const struct bytestream_1_vt clobberstream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 clobberstream_as_bytestream_1(clobberstream_t *clstr)
{
    return (bytestream_1) { clstr, &clobberstream_vt };
}

FSTRACE_DECL(ASYNC_CLOBBERSTREAM_CREATE,
             "UID=%64u PTR=%p ASYNC=%p STREAM=%p OFFSET=%z MASK=0x%64x");

clobberstream_t *clobber(async_t *async, bytestream_1 stream, size_t offset,
                         uint64_t mask)
{
    clobberstream_t *clstr = fsalloc(sizeof *clstr);
    clstr->async = async;
    clstr->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_CLOBBERSTREAM_CREATE, clstr->uid, clstr, async, stream.obj,
            offset, mask);
    clstr->stream = stream;
    clstr->cursor = 0;
    clstr->offset = offset;
    int i;
    for (i = 0; i < sizeof clstr->mask; i++) {
        clstr->mask[i] = mask & 0xff;
        mask >>= 8;
    }
    return clstr;
}
