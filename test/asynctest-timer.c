#include "asynctest-timer.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>

#include <async/async.h>

static uint64_t nanoseconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000000ULL + tv.tv_usec * 1000;
}

VERDICT test_async_timer_start(void)
{
    enum { DURATION = 2 };
    async_t *async = make_async();
    async_timer_start(async, async_now(async) + DURATION * ASYNC_S,
                      (action_1) { async, (act_1) async_quit_loop });
    uint64_t t0 = nanoseconds();
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    uint64_t t1 = nanoseconds();
    destroy_async(async);
    uint64_t delta = t1 - t0;
    if (delta < DURATION * 950000000ULL) {
        tlog("Premature timeout");
        return FAIL;
    }
    if (delta > DURATION * 1050000000ULL) {
        tlog("Late timeout");
        return FAIL;
    }
    return posttest_check(PASS);
}

typedef struct {
    async_t *async;
    async_timer_t *cancelable;
} TEST_ASYNC_TIMER_CANCEL;

static void cancel_it(TEST_ASYNC_TIMER_CANCEL *context)
{
    async_timer_cancel(context->async, context->cancelable);
}

VERDICT test_async_timer_cancel(void)
{
    TEST_ASYNC_TIMER_CANCEL context;
    async_t *async = context.async = make_async();
    context.cancelable =
        async_timer_start(async, async_now(async) + 2 * ASYNC_S,
                          (action_1) { async, (act_1) async_quit_loop });
    async_timer_start(async, async_now(async) + 1 * ASYNC_S,
                      (action_1) { &context, (act_1) cancel_it });
    async_timer_start(async, async_now(async) + 3 * ASYNC_S,
                      (action_1) { async, (act_1) async_quit_loop });
    uint64_t t0 = nanoseconds();
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    uint64_t t1 = nanoseconds();
    destroy_async(async);
    uint64_t delta = t1 - t0;
    if (delta < 3 * 950000000ULL) {
        tlog("Premature timeout");
        return FAIL;
    }
    if (delta > 3 * 1050000000ULL) {
        tlog("Late timeout");
        return FAIL;
    }
    return posttest_check(PASS);
}
