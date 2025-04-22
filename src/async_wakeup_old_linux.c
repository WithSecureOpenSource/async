/*
 * The wakeup mechanism for old Linux systems that lack timer_fd
 * support. On such systems, async_poll_2() cannot be implemented.
 *
 * A pipe is created; async->wakeup_fd is the read end and
 * async->wakeup_trigger_fd the write end. The read end is registered
 * with the epoll_fd. Whenever the async main loop needs to be woken
 * up (e.g. by invoking async_execute() from a separate thread), a
 * dummy byte is written into async->wakeup_trigger_fd.
 */

#include <assert.h>
#include <errno.h>
#include <fstrace.h>
#include <unixkit/unixkit.h>

#include "async_imp.h"

#if defined(__linux__) && PIPE_WAKEUP

void async_initialize_wakeup(async_t *async)
{
    async->wakeup_fd = -1;
    async->wakeup_trigger_fd = -1;
}

void async_dismantle_wakeup(async_t *async)
{
    if (async->wakeup_fd >= 0) {
        async_unregister(async, async->wakeup_fd);
        (void) close(async->wakeup_fd);
        (void) close(async->wakeup_trigger_fd);
    }
}

FSTRACE_DECL(ASYNC_WAKE_UP, "UID=%64u");

void async_wake_up(async_t *async)
{
    FSTRACE(ASYNC_WAKE_UP, async->uid);
    if (async->wakeup_trigger_fd >= 0 &&
        write(async->wakeup_trigger_fd, async_wake_up, 1) < 0)
        assert(errno == EAGAIN);
}

void async_cancel_wakeup(async_t *async)
{
}

void async_arm_wakeup(async_t *async)
{
    uint8_t buffer[1024];
    while (read(async->wakeup_fd, buffer, sizeof buffer) > 0)
        ;
}

FSTRACE_DECL(ASYNC_SET_UP_WAKEUP,
             "UID=%64u WAKEUP-FD=%d WAKEUP-TRIGGER-FD=%d");
FSTRACE_DECL(ASYNC_SET_UP_WAKEUP_FAIL, "UID=%64u ERRNO=%e");

bool async_set_up_wakeup(async_t *async)
{
    if (async->wakeup_fd >= 0)
        return true;
    int fds[2];
    if (!unixkit_pipe(fds)) {
        FSTRACE(ASYNC_SET_UP_WAKEUP_FAIL, async->uid);
        return false;
    }
    async->wakeup_fd = fds[0];
    async->wakeup_trigger_fd = fds[1];
    FSTRACE(ASYNC_SET_UP_WAKEUP, async->uid,
            async->wakeup_fd, async->wakeup_trigger_fd);
    async_register_event(async, async->wakeup_fd, ASYNC_SENTINEL_EVENT);
    async_nonblock(async->wakeup_trigger_fd);
    async_arm_wakeup(async);
    return true;
}

#endif
