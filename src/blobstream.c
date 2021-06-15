#include "blobstream.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"

struct blobstream {
    async_t *async;
    uint64_t uid;
    const uint8_t *blob;
    int cursor;
    size_t count;
    action_1 close_action;
};

size_t blobstream_remaining(blobstream_t *blobstr)
{
    return blobstr->count - blobstr->cursor;
}

FSTRACE_DECL(ASYNC_BLOBSTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_BLOBSTREAM_READ_DUMP, "UID=%64u DATA=%B");

ssize_t blobstream_read(blobstream_t *blobstr, void *buf, size_t count)
{
    ssize_t n = blobstream_remaining(blobstr);
    if (n > count)
        n = count;
    if (n > 0)
        memcpy(buf, blobstr->blob + blobstr->cursor, n);
    FSTRACE(ASYNC_BLOBSTREAM_READ, blobstr->uid, count, n);
    FSTRACE(ASYNC_BLOBSTREAM_READ_DUMP, blobstr->uid, buf, n);
    blobstr->cursor += n;
    return n;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return blobstream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_BLOBSTREAM_CLOSE, "UID=%64u");

void blobstream_close(blobstream_t *blobstr)
{
    FSTRACE(ASYNC_BLOBSTREAM_CLOSE, blobstr->uid);
    action_1_perf(blobstr->close_action);
    assert(blobstr->async != NULL);
    async_wound(blobstr->async, blobstr);
    blobstr->async = NULL;
}

static void _close(void *obj)
{
    blobstream_close(obj);
}

FSTRACE_DECL(ASYNC_BLOBSTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void blobstream_register_callback(blobstream_t *blobstr, action_1 action)
{
    FSTRACE(ASYNC_BLOBSTREAM_REGISTER, blobstr->uid, action.obj, action.act);
}

static void _register_callback(void *obj, action_1 action)
{
    blobstream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_BLOBSTREAM_UNREGISTER, "UID=%64u");

void blobstream_unregister_callback(blobstream_t *blobstr)
{
    FSTRACE(ASYNC_BLOBSTREAM_UNREGISTER, blobstr->uid);
}

static void _unregister_callback(void *obj)
{
    blobstream_unregister_callback(obj);
}

static const struct bytestream_1_vt blobstream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 blobstream_as_bytestream_1(blobstream_t *blobstr)
{
    return (bytestream_1) { blobstr, &blobstream_vt };
}

FSTRACE_DECL(ASYNC_BLOBSTREAM_CREATE, "UID=%64u PTR=%p");

static blobstream_t *make_blobstream(async_t *async, uint64_t uid,
                                     const void *blob, size_t count,
                                     action_1 close_action)
{
    blobstream_t *blobstr = fsalloc(sizeof *blobstr);
    FSTRACE(ASYNC_BLOBSTREAM_CREATE, uid, blobstr);
    blobstr->async = async;
    blobstr->uid = uid;
    blobstr->blob = blob;
    blobstr->count = count;
    blobstr->cursor = 0;
    blobstr->close_action = close_action;
    return blobstr;
}

FSTRACE_DECL(ASYNC_BLOBSTREAM_OPEN, "UID=%64u ASYNC=%p BLOB-SIZE=%z");
FSTRACE_DECL(ASYNC_BLOBSTREAM_BLOB_DUMP, "UID=%64u DATA=%B");

blobstream_t *open_blobstream(async_t *async, const void *blob, size_t count)
{
    uint64_t uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_BLOBSTREAM_OPEN, uid, async, count);
    FSTRACE(ASYNC_BLOBSTREAM_BLOB_DUMP, uid, blob, count);
    return make_blobstream(async, uid, (void *) blob, count, NULL_ACTION_1);
}

FSTRACE_DECL(ASYNC_BLOBSTREAM_COPY, "UID=%64u ASYNC=%p BLOB-SIZE=%z");

blobstream_t *copy_blobstream(async_t *async, const void *blob, size_t count)
{
    uint64_t uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_BLOBSTREAM_COPY, uid, async, count);
    FSTRACE(ASYNC_BLOBSTREAM_BLOB_DUMP, uid, blob, count);
    void *copy = fsalloc(count);
    memcpy(copy, blob, count);
    return make_blobstream(async, uid, copy, count,
                           (action_1) { copy, fsfree });
}

FSTRACE_DECL(ASYNC_BLOBSTREAM_ADOPT,
             "UID=%64u ASYNC=%p BLOB-SIZE=%z OBJ=%p ACT=%p");

blobstream_t *adopt_blobstream(async_t *async, const void *blob, size_t count,
                               action_1 close_action)
{
    uint64_t uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_BLOBSTREAM_ADOPT, uid, async, count, close_action.obj,
            close_action.act);
    FSTRACE(ASYNC_BLOBSTREAM_BLOB_DUMP, uid, blob, count);
    return make_blobstream(async, uid, blob, count, close_action);
}
