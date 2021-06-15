#include "asynctest-old-school.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <async/async.h>

static int nonblock(int fd)
{
    int status = fcntl(fd, F_GETFL, 0);
    if (status < 0)
        return status;
    return fcntl(fd, F_SETFL, status | O_NONBLOCK);
}

typedef struct {
    tester_base_t base;
    int sd[2];
    enum {
        DONT_READ_YET,
        NOT_YET_EITHER,
        READING,
        SLEEPING,
        DONT_DRAIN_YET,
        DRAINED
    } state;
} tester_t;

static void supply_byte(tester_t *context)
{
    ssize_t count = write(context->sd[1], write, 1);
    assert(count == 1);
}

static void drain_it(tester_t *context)
{
    uint8_t buffer[100];
    ssize_t count;
    do {
        count = read(context->sd[1], buffer, sizeof buffer);
    } while (count >= 0);
    assert(errno == EAGAIN);
    context->state = DRAINED;
}

static void modify_it(tester_t *context)
{
    if (context->state != SLEEPING) {
        tlog("modify_it called from a bad state: %d", (int) context->state);
        quit_test(&context->base);
        return;
    }
    context->state = DONT_DRAIN_YET;
    async_modify_old_school(context->base.async, context->sd[0], 1, 1);
    async_timer_start(context->base.async,
                      async_now(context->base.async) + ASYNC_S,
                      (action_1) { context, (act_1) drain_it });
}

static void probe_it(tester_t *context)
{
    if (!context->base.async)
        return;

    uint8_t buffer[100];
    ssize_t count;
    switch (context->state) {
        case DONT_READ_YET:
            context->state = NOT_YET_EITHER;
            return;
        case NOT_YET_EITHER:
            context->state = READING;
            return;
        case READING:
            count = read(context->sd[0], buffer, sizeof buffer);
            if (count < 0) {
                tlog("Funny errno = %d from read", (int) errno);
                quit_test(&context->base);
                return;
            }
            assert(count == 1);
            do {
                count = write(context->sd[0], buffer, sizeof buffer);
            } while (count >= 0);
            assert(errno == EAGAIN);
            context->state = SLEEPING;
            async_timer_start(context->base.async,
                              async_now(context->base.async) + ASYNC_S,
                              (action_1) { context, (act_1) modify_it });
            return;
        case SLEEPING:
            tlog("spuriously woken up while SLEEPING");
            quit_test(&context->base);
            return;
        case DONT_DRAIN_YET:
            tlog("spuriously woken up while in DONT_DRAIN_YET");
            quit_test(&context->base);
            return;
        case DRAINED:
            context->base.verdict = PASS;
            quit_test(&context->base);
            return;
        default:
            tlog("Spurious probe");
    }
}

VERDICT test_async_old_school(void)
{
    async_t *async = make_async();
    tester_t context = { .state = DONT_READ_YET };
    init_test(&context.base, async, 5);
    int status = socketpair(AF_UNIX, SOCK_STREAM, 0, context.sd);
    assert(status >= 0);
    nonblock(context.sd[0]);
    nonblock(context.sd[1]);
    async_register_old_school(async, context.sd[0],
                              (action_1) { &context, (act_1) probe_it });
    async_execute(async, (action_1) { &context, (act_1) supply_byte });
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    async_unregister(async, context.sd[0]);
    close(context.sd[0]);
    close(context.sd[1]);
    destroy_async(async);
    return posttest_check(context.base.verdict);
}
