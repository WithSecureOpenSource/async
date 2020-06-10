#include <unistd.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <pthread.h>
#include <async/async.h>
#include "asynctest-old-school.h"

static int nonblock(int fd)
{
    int status = fcntl(fd, F_GETFL, 0);
    if (status < 0)
        return status;
    return fcntl(fd, F_SETFL, status | O_NONBLOCK);
}

typedef struct {
    async_t *async;
    int sd[2];
    enum {
        DONT_READ_YET, NOT_YET_EITHER, READING, SLEEPING,
        DONT_DRAIN_YET, DRAINED, FAILED, PASSED
    } state;
} REGISTER_CONTEXT;

static void supply_byte(REGISTER_CONTEXT *context)
{
    ssize_t count = write(context->sd[1], write, 1);
    assert(count == 1);
}

static void drain_it(REGISTER_CONTEXT *context)
{
    uint8_t buffer[100];
    ssize_t count;
    do {
        count = read(context->sd[1], buffer, sizeof buffer);
    } while (count >= 0);
    assert(errno == EAGAIN);
    context->state = DRAINED;
}

static void modify_it(REGISTER_CONTEXT *context)
{
    if (context->state != SLEEPING) {
        tlog("modify_it called from a bad state: %d", (int) context->state);
        async_quit_loop(context->async);
        return;
    }
    context->state = DONT_DRAIN_YET;
    async_modify_old_school(context->async, context->sd[0], 1, 1);
    async_timer_start(context->async,
                      async_now(context->async) + ASYNC_S,
                      (action_1) { context, (act_1) drain_it });
}


static void fail_it(REGISTER_CONTEXT *context)
{
    context->state = FAILED;
    async_quit_loop(context->async);
}

static void probe_it(REGISTER_CONTEXT *context)
{
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
                context->state = FAILED;
                async_quit_loop(context->async);
                return;
            }
            assert(count == 1);
            do {
                count = write(context->sd[0], buffer, sizeof buffer);
            } while (count >= 0);
            assert(errno == EAGAIN);
            context->state = SLEEPING;
            async_timer_start(context->async,
                              async_now(context->async) + ASYNC_S,
                              (action_1) { context, (act_1) modify_it });
            return;
        case SLEEPING:
            tlog("spuriously woken up while SLEEPING");
            async_quit_loop(context->async);
            return;
        case DONT_DRAIN_YET:
            tlog("spuriously woken up while in DONT_DRAIN_YET");
            async_quit_loop(context->async);
            return;
        case DRAINED:
            context->state = PASSED;
            async_quit_loop(context->async);
            return;
        default:
            tlog("Spurious probe");
    }
}

VERDICT test_async_old_school(void)
{
    REGISTER_CONTEXT context = { .state = DONT_READ_YET };
    int status = socketpair(AF_UNIX, SOCK_STREAM, 0, context.sd);
    assert(status >= 0);
    nonblock(context.sd[0]);
    nonblock(context.sd[1]);
    async_t *async = context.async = make_async();
    async_register_old_school(async, context.sd[0],
                              (action_1) { &context, (act_1) probe_it });
    async_execute(async, (action_1) { &context, (act_1) supply_byte });
    async_timer_start(async, async_now(async) + 5 * ASYNC_S,
                      (action_1) { async, (act_1) fail_it });
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    async_unregister(async, context.sd[0]);
    close(context.sd[0]);
    close(context.sd[1]);
    destroy_async(async);
    switch (context.state) {
        case PASSED:
            return posttest_check(PASS);
        default:
            return FAIL;
    }
}
