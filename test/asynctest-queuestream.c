#include <errno.h>
#include <assert.h>
#include <async/async.h>
#include <async/queuestream.h>
#include <async/stringstream.h>
#include "asynctest-queuestream.h"

enum {
    QSTR_INIT,
    QSTR_HELLO,
    QSTR_QUIET,
    QSTR_WORLD,
    QSTR_TERMINATED,
    QSTR_DONE
};

typedef struct {
    async_t *async;
    queuestream_t *qstr;
    int state, verdict;
    size_t offset;
} QSTR_CONTEXT;

static void qstr_probe(QSTR_CONTEXT *context);

static void qstr_init(QSTR_CONTEXT *context, ssize_t count,
                      const uint8_t buffer[])
{
    if (count >= 0 || errno != EAGAIN) {
        tlog("Expected EAGAIN, got %d (errno = %d)",
             (int) count, (int) errno);
        async_quit_loop(context->async);
        return;
    }
    stringstream_t *stringstr = open_stringstream(context->async, "Hello");
    queuestream_enqueue(context->qstr,
                        stringstream_as_bytestream_1(stringstr));
    context->state = QSTR_HELLO;
}

static void qstr_hello(QSTR_CONTEXT *context, ssize_t count,
                       const uint8_t buffer[])
{
    if (count <= 0) {
        tlog("Unexpected error %d (errno %d) from queuestream",
             (int) count, (int) errno);
        async_quit_loop(context->async);
        return;
    }
    context->offset += count;
    if (context->offset < 5) {
        async_execute(context->async,
                      (action_1) { context, (act_1) qstr_probe });
        return;
    }
    if (context->offset > 5) {
        tlog("Too many bytes from queuestream");
        async_quit_loop(context->async);
        return;
    }
    context->offset = 0;
    context->state = QSTR_QUIET;
    async_execute(context->async,
                  (action_1) { context, (act_1) qstr_probe });
}

static void qstr_quiet(QSTR_CONTEXT *context, ssize_t count,
                       const uint8_t buffer[])
{
    if (count >= 0 || errno != EAGAIN) {
        tlog("Expected EAGAIN, got %d (errno = %d)",
             (int) count, (int) errno);
        async_quit_loop(context->async);
        return;
    }
    stringstream_t *stringstr = open_stringstream(context->async, " world");
    queuestream_enqueue(context->qstr,
                        stringstream_as_bytestream_1(stringstr));
    context->state = QSTR_WORLD;
    queuestream_terminate(context->qstr);
}

static void qstr_world(QSTR_CONTEXT *context, ssize_t count,
                       const uint8_t buffer[])
{
    if (count <= 0) {
        tlog("Unexpected error %d (errno %d) from queuestream",
             (int) count, (int) errno);
        async_quit_loop(context->async);
        return;
    }
    context->offset += count;
    if (context->offset < 6) {
        async_execute(context->async,
                      (action_1) { context, (act_1) qstr_probe });
        return;
    }
    if (context->offset > 6) {
        tlog("Too many bytes from queuestream");
        async_quit_loop(context->async);
        return;
    }
    context->state = QSTR_TERMINATED;
    async_execute(context->async,
                  (action_1) { context, (act_1) qstr_probe });
}

static void qstr_terminated(QSTR_CONTEXT *context, ssize_t count,
                            const uint8_t buffer[])
{
    if (count != 0) {
        tlog("Unexpected error %d (errno %d) from queuestream",
             (int) count, (int) errno);
        async_quit_loop(context->async);
        return;
    }
    context->state = QSTR_DONE;
    context->verdict = PASS;
    action_1 quit = { context->async, (act_1) async_quit_loop };
    async_execute(context->async, quit);
}

static void qstr_probe(QSTR_CONTEXT *context)
{
    if (context->state == QSTR_DONE) /* spurious? */
        return;
    uint8_t buffer[100];
    ssize_t count = queuestream_read(context->qstr,
                                     buffer + context->offset,
                                     sizeof buffer - context->offset);
    switch (context->state) {
        case QSTR_INIT:
            qstr_init(context, count, buffer);
            break;
        case QSTR_HELLO:
            qstr_hello(context, count, buffer);
            break;
        case QSTR_QUIET:
            qstr_quiet(context, count, buffer);
            break;
        case QSTR_WORLD:
            qstr_world(context, count, buffer);
            break;
        case QSTR_TERMINATED:
            qstr_terminated(context, count, buffer);
            break;
        default:
            assert(0);
    }
}

VERDICT test_queuestream(void)
{
    QSTR_CONTEXT context = {
        .state = QSTR_INIT,
        .verdict = FAIL
    };
    async_t *async = context.async = make_async();
    queuestream_t *qstr = context.qstr = make_queuestream(async);
    action_1 qstr_probe_cb = { &context, (act_1) qstr_probe };
    queuestream_register_callback(qstr, qstr_probe_cb);
    async_execute(async, qstr_probe_cb);
    async_timer_start(async, async_now(async) + 2 * ASYNC_S,
                      (action_1) { async, (act_1) async_quit_loop });
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    queuestream_close(qstr);
    destroy_async(async);
    return posttest_check(context.verdict);
}

VERDICT test_relaxed_queuestream(void)
{
    QSTR_CONTEXT context = {
        .state = QSTR_INIT,
        .verdict = FAIL
    };
    async_t *async = context.async = make_async();
    queuestream_t *qstr = context.qstr = make_relaxed_queuestream(async);
    action_1 qstr_probe_cb = { &context, (act_1) qstr_probe };
    queuestream_register_callback(qstr, qstr_probe_cb);
    async_execute(async, qstr_probe_cb);
    async_timer_start(async, async_now(async) + 2 * ASYNC_S,
                      (action_1) { async, (act_1) async_quit_loop });
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    if (context.verdict != PASS)
        return FAIL;
    if (queuestream_closed(qstr)) {
        tlog("Relaxed queuestream claims it is closed");
        return FAIL;
    }
    queuestream_close(qstr);
    if (!queuestream_closed(qstr)) {
        tlog("Relaxed queuestream claims it is not closed");
        return FAIL;
    }
    queuestream_release(qstr);
    destroy_async(async);
    return posttest_check(context.verdict);
}
