#include "drystream.h"

#include <errno.h>
#include <stdio.h>

#include <fstrace.h>

#include "async_version.h"

FSTRACE_DECL(ASYNC_DRYSTREAM_READ, "WANT=%z GOT=-1 ERRNO=%e");

static ssize_t _read(void *obj, void *buf, size_t count)
{
    errno = EAGAIN;
    FSTRACE(ASYNC_DRYSTREAM_READ, count);
    return -1;
}

static void _close(void *obj) {}

static void _register_callback(void *obj, action_1 action) {}

static void _unregister_callback(void *obj) {}

static const struct bytestream_1_vt drystream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

const bytestream_1 drystream = {
    .obj = NULL,
    .vt = &drystream_vt,
};
