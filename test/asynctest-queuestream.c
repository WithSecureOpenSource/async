#include <errno.h>
#include <assert.h>
#include <string.h>
#include <async/async.h>
#include <async/queuestream.h>
#include <async/stringstream.h>
#include "asynctest-queuestream.h"

enum {
    QSTR_ENQUEUE_INPUT,
    QSTR_READ_INPUT,
    QSTR_TERMINATED,
    QSTR_DONE
};

typedef struct {
    async_t *async;
    queuestream_t *qstr;
    int state, verdict;
    size_t offset;
    uint8_t buffer[100];
    const char **input;
} QSTR_CONTEXT;

static const char *INPUTS[] = {
    "Hello",
    " world",
    NULL,
};

static void qstr_probe(QSTR_CONTEXT *context);

static void qstr_enqueue_input(QSTR_CONTEXT *context, ssize_t count)
{
    if (count >= 0 || errno != EAGAIN) {
        tlog("Expected EAGAIN, got %d (errno = %d)",
             (int) count, (int) errno);
        async_quit_loop(context->async);
        return;
    }
    if (*context->input) {
        stringstream_t *stringstr =
            open_stringstream(context->async, *context->input);
        queuestream_enqueue(context->qstr,
                            stringstream_as_bytestream_1(stringstr));
        context->state = QSTR_READ_INPUT;
    } else {
        queuestream_terminate(context->qstr);
        context->state = QSTR_TERMINATED;
    }
}

static void qstr_read_input(QSTR_CONTEXT *context, ssize_t count)
{
    if (count <= 0) {
        tlog("Unexpected error %d (errno %d) from queuestream",
             (int) count, (int) errno);
        async_quit_loop(context->async);
        return;
    }
    size_t len = strlen(*context->input);
    context->offset += count;
    if (context->offset < len) {
        async_execute(context->async,
                      (action_1) { context, (act_1) qstr_probe });
        return;
    }
    if (context->offset > len) {
        tlog("Too many bytes from queuestream");
        async_quit_loop(context->async);
        return;
    }
    if (memcmp(*context->input, context->buffer, len)) {
        tlog("Input mismatch");
        async_quit_loop(context->async);
        return;
    }
    context->offset = 0;
    context->input++;
    context->state = QSTR_ENQUEUE_INPUT;
    async_execute(context->async,
                  (action_1) { context, (act_1) qstr_probe });
}

static void qstr_terminated(QSTR_CONTEXT *context, ssize_t count)
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
    ssize_t count = queuestream_read(context->qstr,
                                     context->buffer + context->offset,
                                     sizeof context->buffer - context->offset);
    switch (context->state) {
        case QSTR_ENQUEUE_INPUT:
            qstr_enqueue_input(context, count);
            break;
        case QSTR_READ_INPUT:
            qstr_read_input(context, count);
            break;
        case QSTR_TERMINATED:
            qstr_terminated(context, count);
            break;
        default:
            assert(0);
    }
}

VERDICT test_queuestream(void)
{
    QSTR_CONTEXT context = {
        .state = QSTR_ENQUEUE_INPUT,
        .input = INPUTS,
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
        .state = QSTR_ENQUEUE_INPUT,
        .input = INPUTS,
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
