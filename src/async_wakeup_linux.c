/*
 * The wakeup mechanism for Linux systems that support timer file
 * descriptors. The timer_fd is placed in async->wakeupfd. When it
 * expires, the timer_fd causes async's epoll_fd to become readable.
 */

#include <assert.h>
#include <errno.h>
#include <fstrace.h>
#include <unixkit/unixkit.h>

#include "async_imp.h"

#if defined(__linux__) && !PIPE_WAKEUP

#include <sys/timerfd.h>

void async_initialize_wakeup(async_t *async)
{
    async->wakeup_fd = -1;
}

void async_dismantle_wakeup(async_t *async)
{
    if (async->wakeup_fd >= 0) {
        async_unregister(async, async->wakeup_fd);
        (void) close(async->wakeup_fd);
    }
}

static void set_wakeup_time(async_t *async, const struct itimerspec *target)
{
    if (timerfd_settime(async->wakeup_fd, TFD_TIMER_ABSTIME, target, NULL) < 0)
        assert(false);
}

FSTRACE_DECL(ASYNC_WAKE_UP, "UID=%64u");

void async_wake_up(async_t *async)
{
    FSTRACE(ASYNC_WAKE_UP, async->uid);
    if (async->wakeup_fd >= 0) {
        static const struct itimerspec immediate = {
            .it_value = {
                .tv_nsec = 1,   /* in the past but not zero */
            },
        };
        set_wakeup_time(async, &immediate);
    }
}

void async_cancel_wakeup(async_t *async)
{
    static const struct itimerspec never = { 0 };
    set_wakeup_time(async, &never);
}

void async_schedule_wakeup(async_t *async, uint64_t expires)
{
    uint64_t expires_s = expires / 1000000000;
    if (sizeof(time_t) < 5 && expires_s > INT32_MAX)
        expires_s = INT32_MAX;
    struct itimerspec target = {
        .it_value = {
            .tv_sec = expires_s,
            /* Make sure .it_value doesn't become 0 (= never). */
            .tv_nsec = expires % 1000000000 | !expires_s,
        },
    };
    set_wakeup_time(async, &target);
}

void async_arm_wakeup(async_t *async)
{
    uint8_t buffer[1024];
    while (read(async->wakeup_fd, buffer, sizeof buffer) > 0)
        ;
}

FSTRACE_DECL(ASYNC_SET_UP_WAKEUP, "UID=%64u WAKEUP-FD=%d");
FSTRACE_DECL(ASYNC_SET_UP_WAKEUP_FAIL, "UID=%64u ERRNO=%e");

bool async_set_up_wakeup(async_t *async)
{
    if (async->wakeup_fd >= 0)
        return true;
    async->wakeup_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (async->wakeup_fd < 0) {
        FSTRACE(ASYNC_SET_UP_WAKEUP_FAIL, async->uid);
        return false;
    }
    FSTRACE(ASYNC_SET_UP_WAKEUP, async->uid, async->wakeup_fd);
    async_register_event(async, async->wakeup_fd, ASYNC_SENTINEL_EVENT);
    return true;
}

#endif
