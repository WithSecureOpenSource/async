#include "zerostream.h"

#include <string.h>

#include <fstrace.h>

#include "async_version.h"

FSTRACE_DECL(ASYNC_ZEROSTREAM_READ, "WANT=%z GOT=%z");

static ssize_t _read(void *obj, void *buf, size_t count)
{
    memset(buf, 0, count);
    FSTRACE(ASYNC_ZEROSTREAM_READ, count, count);
    return count;
}

static void _close(void *obj) {}

static void _register_callback(void *obj, action_1 action) {}

static void _unregister_callback(void *obj) {}

static const struct bytestream_1_vt zerostream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

const bytestream_1 zerostream = {
    .obj = NULL,
    .vt = &zerostream_vt,
};
