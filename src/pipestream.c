#include "pipestream.h"

#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"

struct pipestream {
    async_t *async;
    uint64_t uid;
    action_1 callback;
    int fd;
};

FSTRACE_DECL(ASYNC_PIPESTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_PIPESTREAM_READ_DUMP, "UID=%64u DATA=%B");

ssize_t pipestream_read(pipestream_t *pipestr, void *buf, size_t count)
{
    ssize_t n = read(pipestr->fd, buf, count);
    FSTRACE(ASYNC_PIPESTREAM_READ, pipestr->uid, count, n);
    FSTRACE(ASYNC_PIPESTREAM_READ_DUMP, pipestr->uid, buf, n);
    return n;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return pipestream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_PIPESTREAM_CLOSE, "UID=%64u");

void pipestream_close(pipestream_t *pipestr)
{
    FSTRACE(ASYNC_PIPESTREAM_CLOSE, pipestr->uid);
    assert(pipestr->async != NULL);
    async_unregister(pipestr->async, pipestr->fd);
    close(pipestr->fd);
    pipestr->callback = NULL_ACTION_1;
    async_wound(pipestr->async, pipestr);
    pipestr->async = NULL;
}

static void _close(void *obj)
{
    pipestream_close(obj);
}

FSTRACE_DECL(ASYNC_PIPESTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void pipestream_register_callback(pipestream_t *pipestr, action_1 action)
{
    FSTRACE(ASYNC_PIPESTREAM_REGISTER, pipestr->uid, action.obj, action.act);
    pipestr->callback = action;
}

static void _register_callback(void *obj, action_1 action)
{
    pipestream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_PIPESTREAM_UNREGISTER, "UID=%64u");

void pipestream_unregister_callback(pipestream_t *pipestr)
{
    FSTRACE(ASYNC_PIPESTREAM_UNREGISTER, pipestr->uid);
    pipestr->callback = NULL_ACTION_1;
}

static void _unregister_callback(void *obj)
{
    pipestream_unregister_callback(obj);
}

static const struct bytestream_1_vt pipestream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 pipestream_as_bytestream_1(pipestream_t *pipestr)
{
    return (bytestream_1) { pipestr, &pipestream_vt };
}

static void probe(pipestream_t *pipestr)
{
    action_1_perf(pipestr->callback);
}

FSTRACE_DECL(ASYNC_PIPESTREAM_CREATE, "UID=%64u PTR=%p ASYNC=%p FD=%d");

pipestream_t *open_pipestream(async_t *async, int fd)
{
    pipestream_t *pipestr = fsalloc(sizeof *pipestr);
    pipestr->async = async;
    pipestr->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_PIPESTREAM_CREATE, pipestr->uid, pipestr, async, fd);
    pipestr->callback = NULL_ACTION_1;
    pipestr->fd = fd;
    async_register(async, fd, (action_1) { pipestr, (act_1) probe });
    return pipestr;
}
