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
    tester_base_t base;
    int sd[2];
    enum { READING, WRITING, DONE } state;
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
}

static void probe_it(tester_t *context)
{
    if (!context->base.async) {
        context->state = DONE;
        return;
    }

    uint8_t buffer[100];
    ssize_t count;
    switch (context->state) {
        case READING:
            count = read(context->sd[0], buffer, sizeof buffer);
            if (count < 0) {
                if (errno == EAGAIN)
                    return;
                tlog("Funny errno = %d from read", (int) errno);
                context->state = DONE;
                quit_test(&context->base);
                return;
            }
            assert(count == 1);
            do {
                count = write(context->sd[1], buffer, sizeof buffer);
            } while (count >= 0);
            assert(errno == EAGAIN);
            async_execute(context->base.async,
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
            context->state = DONE;
            context->base.verdict = PASS;
            quit_test(&context->base);
            return;
        default:
            tlog("Spurious probe");
    }
}

VERDICT test_async_register(void)
{
    async_t *async = make_async();
    tester_t context = { .state = READING };
    init_test(&context.base, async, 1);
    int status = socketpair(AF_UNIX, SOCK_STREAM, 0, context.sd);
    assert(status >= 0);
    nonblock(context.sd[1]);
    async_register(async, context.sd[0],
                   (action_1) { &context, (act_1) probe_it });
    uint8_t buffer[100];
    ssize_t count = read(context.sd[0], buffer, sizeof buffer);
    assert(count < 0 && errno == EAGAIN);
    async_execute(async, (action_1) { &context, (act_1) supply_byte });
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    async_unregister(async, context.sd[0]);
    close(context.sd[0]);
    close(context.sd[1]);
    destroy_async(async);
    return posttest_check(context.base.verdict);
}

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    tester_base_t base;
    int counter;
} mt_tester_t;

enum { MT_COUNT = 5 };

static void mt_timeout(mt_tester_t *context)
{
    if (++context->counter == MT_COUNT)
        context->base.verdict = PASS;
}

static void *aux_thread(void *arg)
{
    mt_tester_t *context = arg;
    int i;
    for (i = 0; i < MT_COUNT; i++) {
        pthread_mutex_lock(&mutex);
        async_timer_start(context->base.async,
                          async_now(context->base.async) + ASYNC_S,
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
    async_t *async = make_async();
    mt_tester_t context = {};
    init_test(&context.base, async, MT_COUNT + 2);
    pthread_create(&thread, &attr, aux_thread, &context);
    pthread_mutex_lock(&mutex);
    if (async_loop_protected(async,
                             lock,
                             unlock,
                             &mutex) < 0)
        tlog("Unexpected error from async_loop_protected: %d", errno);
    pthread_mutex_unlock(&mutex);
    destroy_async(async);
    pthread_join(thread, NULL);
    return posttest_check(context.base.verdict);
}

VERDICT test_async_poll(void)
{
    async_t *async = make_async();
    tester_t context = { .state = READING };
    init_test(&context.base, async, 1);
    int status = socketpair(AF_UNIX, SOCK_STREAM, 0, context.sd);
    assert(status >= 0);
    nonblock(context.sd[1]);
    async_register(async, context.sd[0],
                   (action_1) { &context, (act_1) probe_it });
    uint8_t buffer[100];
    ssize_t count = read(context.sd[0], buffer, sizeof buffer);
    assert(count < 0 && errno == EAGAIN);
    async_execute(async, (action_1) { &context, (act_1) supply_byte });
    int fd = async_fd(async);
    while (context.state != DONE) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        uint64_t timeout;
        int status = async_poll(async, &timeout);
        if (status < 0) {
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
    return posttest_check(context.base.verdict);
}

