#include "blockingstream.h"

#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"

struct blockingstream {
    async_t *async;
    uint64_t uid;
    int fd;
};

FSTRACE_DECL(ASYNC_BLOCKINGSTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_BLOCKINGSTREAM_READ_DUMP, "UID=%64u DATA=%B");

ssize_t blockingstream_read(blockingstream_t *blockingstr, void *buf,
                            size_t count)
{
    ssize_t n = read(blockingstr->fd, buf, count);
    FSTRACE(ASYNC_BLOCKINGSTREAM_READ, blockingstr->uid, count, n);
    FSTRACE(ASYNC_BLOCKINGSTREAM_READ_DUMP, blockingstr->uid, buf, n);
    return n;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return blockingstream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_BLOCKINGSTREAM_CLOSE, "UID=%64u");

void blockingstream_close(blockingstream_t *blockingstr)
{
    FSTRACE(ASYNC_BLOCKINGSTREAM_CLOSE, blockingstr->uid);
    close(blockingstr->fd);
    assert(blockingstr->async != NULL);
    async_wound(blockingstr->async, blockingstr);
    blockingstr->async = NULL;
}

static void _close(void *obj)
{
    blockingstream_close(obj);
}

FSTRACE_DECL(ASYNC_BLOCKINGSTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void blockingstream_register_callback(blockingstream_t *blockingstr,
                                      action_1 action)
{
    FSTRACE(ASYNC_BLOCKINGSTREAM_REGISTER, blockingstr->uid, action.obj,
            action.act);
}

static void _register_callback(void *obj, action_1 action)
{
    blockingstream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_BLOCKINGSTREAM_UNREGISTER, "UID=%64u");

void blockingstream_unregister_callback(blockingstream_t *blockingstr)
{
    FSTRACE(ASYNC_BLOCKINGSTREAM_UNREGISTER, blockingstr->uid);
}

static void _unregister_callback(void *obj)
{
    blockingstream_unregister_callback(obj);
}

static const struct bytestream_1_vt blockingstream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 blockingstream_as_bytestream_1(blockingstream_t *blockingstr)
{
    return (bytestream_1) { blockingstr, &blockingstream_vt };
}

FSTRACE_DECL(ASYNC_BLOCKINGSTREAM_OPEN, "UID=%64u PTR=%p ASYNC=%p FD=%d");

blockingstream_t *open_blockingstream(async_t *async, int fd)
{
    blockingstream_t *blockingstr = fsalloc(sizeof *blockingstr);
    blockingstr->async = async;
    blockingstr->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_BLOCKINGSTREAM_OPEN, blockingstr->uid, blockingstr, async,
            fd);
    blockingstr->fd = fd;
    return blockingstr;
}
