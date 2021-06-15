#include "errorstream.h"

#include <errno.h>
#include <unistd.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"

struct errorstream {
    async_t *async;
    uint64_t uid;
    int err;
    action_1 callback;
};

FSTRACE_DECL(ASYNC_ERRORSTREAM_CREATE, "UID=%64u PTR=%p ASYNC=%p ERRNO=%E");

errorstream_t *make_errorstream(async_t *async, int err)
{
    errorstream_t *estr = fsalloc(sizeof *estr);
    estr->async = async;
    estr->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_ERRORSTREAM_CREATE, estr->uid, estr, async, err);
    estr->err = err;
    estr->callback = NULL_ACTION_1;
    return estr;
}

FSTRACE_DECL(ASYNC_ERRORSTREAM_READ, "UID=%64u WANT=%z GOT=-1 ERRNO=%e");

ssize_t errorstream_read(errorstream_t *estr, void *buf, size_t count)
{
    errno = estr->err;
    FSTRACE(ASYNC_ERRORSTREAM_READ, estr->uid, count);
    return -1;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return errorstream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_ERRORSTREAM_CLOSE, "UID=%64u");

void errorstream_close(errorstream_t *estr)
{
    FSTRACE(ASYNC_ERRORSTREAM_CLOSE, estr->uid);
    async_wound(estr->async, estr);
    estr->async = NULL;
}

static void _close(void *obj)
{
    errorstream_close(obj);
}

FSTRACE_DECL(ASYNC_ERRORSTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void errorstream_register_callback(errorstream_t *estr, action_1 action)
{
    FSTRACE(ASYNC_ERRORSTREAM_REGISTER, estr->uid, action.obj, action.act);
    estr->callback = action;
}

static void _register_callback(void *obj, action_1 action)
{
    errorstream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_ERRORSTREAM_UNREGISTER, "UID=%64u");

void errorstream_unregister_callback(errorstream_t *estr)

{
    FSTRACE(ASYNC_ERRORSTREAM_UNREGISTER, estr->uid);
    estr->callback = NULL_ACTION_1;
}

static void _unregister_callback(void *obj)
{
    errorstream_unregister_callback(obj);
}

static const struct bytestream_1_vt errorstream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 errorstream_as_bytestream_1(errorstream_t *estr)
{
    return (bytestream_1) { estr, &errorstream_vt };
}
