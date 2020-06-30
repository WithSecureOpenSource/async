#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <pthread.h>
#include <async/async.h>
#include "asynctest-poll.h"

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
    enum { READING, WRITING, FAILED, PASSED } state;
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
        case READING:
            count = read(context->sd[0], buffer, sizeof buffer);
            if (count < 0) {
                if (errno == EAGAIN)
                    return;
                tlog("Funny errno = %d from read", (int) errno);
                context->state = FAILED;
                async_quit_loop(context->async);
                return;
            }
            assert(count == 1);
            do {
                count = write(context->sd[1], buffer, sizeof buffer);
            } while (count >= 0);
            assert(errno == EAGAIN);
            async_execute(context->async,
                          (action_1) { context, (act_1) drain_it });
            context->state = WRITING;
            return;
        case WRITING:
            count = write(context->sd[0], buffer, 1);
            if (count != 1) {
                tlog("Write returned %d (errno = %d)",
                     (int) count, (int) errno);
                return;
            }
            context->state = PASSED;
            {
                action_1 quit = { context->async, (act_1) async_quit_loop };
                async_execute(context->async, quit);
            }
            return;
        default:
            tlog("Spurious probe");
    }
}

VERDICT test_async_register(void)
{
    REGISTER_CONTEXT context = { .state = READING };
    int status = socketpair(AF_UNIX, SOCK_STREAM, 0, context.sd);
    assert(status >= 0);
    nonblock(context.sd[1]);
    async_t *async = context.async = make_async();
    async_register(async, context.sd[0],
                   (action_1) { &context, (act_1) probe_it });
    uint8_t buffer[100];
    ssize_t count = read(context.sd[0], buffer, sizeof buffer);
    assert(count < 0 && errno == EAGAIN);
    async_execute(async, (action_1) { &context, (act_1) supply_byte });
    async_timer_start(async, async_now(async) + 1 * ASYNC_S,
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

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    async_t *async;
    int counter;
    int verdict;
} MULTITHREAD_CONTEXT;

enum { MT_COUNT = 5 };

static void mt_timeout(MULTITHREAD_CONTEXT *context)
{
    if (++context->counter == MT_COUNT)
        context->verdict = PASS;
}

static void *aux_thread(void *arg)
{
    MULTITHREAD_CONTEXT *context = arg;
    int i;
    for (i = 0; i < MT_COUNT; i++) {
        pthread_mutex_lock(&mutex);
        async_timer_start(context->async,
                          async_now(context->async) + ASYNC_S,
                          (action_1) { context, (act_1) mt_timeout });
        pthread_mutex_unlock(&mutex);
        sleep(1);
    }
    return NULL;
}

static void lock(void *ptr)
{
    pthread_mutex_t *mutex = ptr;
    pthread_mutex_lock(mutex);
}

static void unlock(void *ptr)
{
    pthread_mutex_t *mutex = ptr;
    pthread_mutex_unlock(mutex);
}

VERDICT test_async_loop_protected(void)
{
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    MULTITHREAD_CONTEXT context = { .verdict = FAIL };
    async_t *async = context.async = make_async();
    pthread_create(&thread, &attr, aux_thread, &context);
    pthread_mutex_lock(&mutex);
    async_timer_start(async, async_now(async) + (MT_COUNT + 2) * ASYNC_S,
                      (action_1) { async, (act_1) async_quit_loop });
    if (async_loop_protected(async,
                             lock,
                             unlock,
                             &mutex) < 0)
        tlog("Unexpected error from async_loop_protected: %d", errno);
    pthread_mutex_unlock(&mutex);
    destroy_async(async);
    pthread_join(thread, NULL);
    return posttest_check(context.verdict);
}

VERDICT test_async_poll(void)
{
    REGISTER_CONTEXT context = { .state = READING };
    int status = socketpair(AF_UNIX, SOCK_STREAM, 0, context.sd);
    assert(status >= 0);
    nonblock(context.sd[1]);
    async_t *async = context.async = make_async();
    async_register(async, context.sd[0],
                   (action_1) { &context, (act_1) probe_it });
    uint8_t buffer[100];
    ssize_t count = read(context.sd[0], buffer, sizeof buffer);
    assert(count < 0 && errno == EAGAIN);
    async_execute(async, (action_1) { &context, (act_1) supply_byte });
    async_timer_start(async, async_now(async) + 1 * ASYNC_S,
                      (action_1) { async, (act_1) fail_it });
    int fd = async_fd(async);
    while (context.state != FAILED && context.state != PASSED) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        uint64_t timeout;
        int status = async_poll(async, &timeout);
        if (status < 0) {
            context.state = FAILED;
            break;
        }
        int64_t delta = timeout - async_now(async);
        struct timeval tv;
        if (delta < 0)
            tv.tv_sec = tv.tv_usec = 0;
        else {
            tv.tv_sec = delta / ASYNC_S;
            tv.tv_usec = delta % ASYNC_S / ASYNC_US;
        }
        select(fd + 1, &fds, NULL, NULL, &tv);
    }
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

