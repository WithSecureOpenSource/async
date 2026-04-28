#include "dryyield.h"

#include <errno.h>
#include <stdio.h>

#include <fstrace.h>

#include "async_version.h"

FSTRACE_DECL(ASYNC_DRYYIELD_RECEIVE, "ERRNO=%e");

static void *_receive(void *obj)
{
    errno = EAGAIN;
    FSTRACE(ASYNC_DRYYIELD_RECEIVE);
    return NULL;
}

static void _close(void *obj) {}

static void _register_callback(void *obj, action_1 action) {}

static void _unregister_callback(void *obj) {}

static const struct yield_1_vt dryyield_vt = {
    .receive = _receive,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

const yield_1 dryyield = {
    .obj = NULL,
    .vt = &dryyield_vt,
};
