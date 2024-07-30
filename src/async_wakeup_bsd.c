/*
 * The wakeup mechanism for BSD systems. An EVFILT_TIMER event is
 * registered. When it expires, async's kqueue file descriptor becomes
 * readable.
 */

#include <assert.h>
#include <errno.h>
#include <fstrace.h>
#include <unixkit/unixkit.h>

#include "async_imp.h"

#ifndef __linux__

#include <sys/event.h>

void async_initialize_wakeup(async_t *async)
{
    async->wakeup_needed = false;
}

void async_cancel_wakeup(async_t *async)
{
    struct kevent event;
    EV_SET(&event, 0, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    (void) kevent(async->poll_fd, &event, 1, NULL, 0, NULL);
}

void async_dismantle_wakeup(async_t *async)
{
    async_cancel_wakeup(async);
}

static void set_wakeup_time(async_t *async, int64_t delay)
{
    struct kevent event;
    EV_SET(&event, 0, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT,
           NOTE_NSECONDS, delay, NULL);
    if (kevent(async->poll_fd, &event, 1, NULL, 0, NULL) < 0)
        assert(false);
}

FSTRACE_DECL(ASYNC_WAKE_UP, "UID=%64u");

void async_wake_up(async_t *async)
{
    FSTRACE(ASYNC_WAKE_UP, async->uid);
    if (async->wakeup_needed)
        set_wakeup_time(async, 0);
}

void async_schedule_wakeup(async_t *async, uint64_t expires)
{
    uint64_t now = async_now(async);
    if (expires <= now)
        set_wakeup_time(async, 0);
    else {
        int64_t delay = expires - now;
        set_wakeup_time(async, delay >= 0 ? delay : INT64_MAX);
    }    
}

void async_arm_wakeup(async_t *async)
{
}

FSTRACE_DECL(ASYNC_SET_UP_WAKEUP, "UID=%64u");

bool async_set_up_wakeup(async_t *async)
{
    FSTRACE(ASYNC_SET_UP_WAKEUP, async->uid);
    async->wakeup_needed = true;
    return true;
}

#endif
