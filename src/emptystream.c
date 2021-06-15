#include "emptystream.h"

#include <stdio.h>

#include <fstrace.h>

#include "async_version.h"

FSTRACE_DECL(ASYNC_EMPTYSTREAM_READ, "WANT=%z GOT=0");

static ssize_t _read(void *obj, void *buf, size_t count)
{
    FSTRACE(ASYNC_EMPTYSTREAM_READ, count);
    return 0;
}

static void _close(void *obj) {}

static void _register_callback(void *obj, action_1 action) {}

static void _unregister_callback(void *obj) {}

static const struct bytestream_1_vt emptystream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

const bytestream_1 emptystream = {
    .obj = NULL,
    .vt = &emptystream_vt,
};
