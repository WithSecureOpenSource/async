#include "stringstream.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"
#include "blobstream.h"

struct stringstream {
    async_t *async;
    uint64_t uid;
    blobstream_t *blobstr;
};

FSTRACE_DECL(ASYNC_STRINGSTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_STRINGSTREAM_READ_DUMP, "UID=%64u TEXT=%A");

ssize_t stringstream_read(stringstream_t *strstr, void *buf, size_t count)
{
    ssize_t n = blobstream_read(strstr->blobstr, buf, count);
    FSTRACE(ASYNC_STRINGSTREAM_READ, strstr->uid, count, n);
    FSTRACE(ASYNC_STRINGSTREAM_READ_DUMP, strstr->uid, buf, n);
    return n;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return stringstream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_STRINGSTREAM_CLOSE, "UID=%64u");

void stringstream_close(stringstream_t *strstr)
{
    FSTRACE(ASYNC_STRINGSTREAM_CLOSE, strstr->uid);
    assert(strstr->async != NULL);
    blobstream_close(strstr->blobstr);
    async_wound(strstr->async, strstr);
    strstr->async = NULL;
}

static void _close(void *obj)
{
    stringstream_close(obj);
}

FSTRACE_DECL(ASYNC_STRINGSTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void stringstream_register_callback(stringstream_t *strstr, action_1 action)
{
    FSTRACE(ASYNC_STRINGSTREAM_REGISTER, strstr->uid, action.obj, action.act);
    blobstream_register_callback(strstr->blobstr, action);
}

static void _register_callback(void *obj, action_1 action)
{
    stringstream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_STRINGSTREAM_UNREGISTER, "UID=%64u");

void stringstream_unregister_callback(stringstream_t *strstr)
{
    FSTRACE(ASYNC_STRINGSTREAM_UNREGISTER, strstr->uid);
    blobstream_unregister_callback(strstr->blobstr);
}

static void _unregister_callback(void *obj)
{
    stringstream_unregister_callback(obj);
}

static const struct bytestream_1_vt stringstream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 stringstream_as_bytestream_1(stringstream_t *strstr)
{
    return (bytestream_1) { strstr, &stringstream_vt };
}

FSTRACE_DECL(ASYNC_STRINGSTREAM_CREATE, "UID=%64u PTR=%p");

static stringstream_t *make_stringstream(async_t *async, uint64_t uid,
                                         blobstream_t *blobstr)
{
    stringstream_t *strstr = fsalloc(sizeof *strstr);
    strstr->async = async;
    strstr->uid = uid;
    FSTRACE(ASYNC_STRINGSTREAM_CREATE, uid, strstr);
    strstr->blobstr = blobstr;
    return strstr;
}

FSTRACE_DECL(ASYNC_STRINGSTREAM_OPEN, "UID=%64u ASYNC=%p LENGTH=%z");
FSTRACE_DECL(ASYNC_STRINGSTREAM_STRING_DUMP, "UID=%64u TEXT=%s");

stringstream_t *open_stringstream(async_t *async, const char *string)
{
    uint64_t uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_STRINGSTREAM_OPEN, uid, async, strlen(string));
    FSTRACE(ASYNC_STRINGSTREAM_STRING_DUMP, uid, string);
    return make_stringstream(async, uid,
                             open_blobstream(async, string, strlen(string)));
}

FSTRACE_DECL(ASYNC_STRINGSTREAM_COPY, "UID=%64u ASYNC=%p LENGTH=%z");

stringstream_t *copy_stringstream(async_t *async, const char *string)
{
    uint64_t uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_STRINGSTREAM_COPY, uid, async, strlen(string));
    FSTRACE(ASYNC_STRINGSTREAM_STRING_DUMP, uid, string);
    return make_stringstream(async, uid,
                             copy_blobstream(async, string, strlen(string)));
}

FSTRACE_DECL(ASYNC_STRINGSTREAM_ADOPT,
             "UID=%64u ASYNC=%p LENGTH=%z OBJ=%p ACT=%p");

stringstream_t *adopt_stringstream(async_t *async, const char *string,
                                   action_1 close_action)
{
    uint64_t uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_STRINGSTREAM_ADOPT, uid, async, strlen(string),
            close_action.obj, close_action.act);
    FSTRACE(ASYNC_STRINGSTREAM_STRING_DUMP, uid, string);
    return make_stringstream(async, uid,
                             adopt_blobstream(async, string, strlen(string),
                                              close_action));
}
