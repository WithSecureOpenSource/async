#include "asynctest-queuestream.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <async/async.h>
#include <async/queuestream.h>
#include <async/stringstream.h>

enum {
    QSTR_ENQUEUE_INPUT,
    QSTR_READ_INPUT,
    QSTR_TERMINATED,
    QSTR_DONE,
};

typedef struct {
    tester_base_t base;
    queuestream_t *qstr;
    int state;
    size_t offset;
    uint8_t buffer[100];
    const char **input;
} tester_t;

static const char *INPUTS[] = {
    "Hello",
    " world",
    NULL,
};

static void qstr_probe(tester_t *context);

static void qstr_enqueue_input(tester_t *context, ssize_t count)
{
    if (count >= 0 || errno != EAGAIN) {
        tlog("Expected EAGAIN, got %d (errno = %d)", (int) count, (int) errno);
        quit_test(&context->base);
        return;
    }
    if (*context->input) {
        stringstream_t *stringstr =
            open_stringstream(context->base.async, *context->input);
        queuestream_enqueue(context->qstr,
                            stringstream_as_bytestream_1(stringstr));
        context->state = QSTR_READ_INPUT;
    } else {
        queuestream_terminate(context->qstr);
        context->state = QSTR_TERMINATED;
    }
}

static void qstr_read_input(tester_t *context, ssize_t count)
{
    if (count <= 0) {
        tlog("Unexpected error %d (errno %d) from queuestream", (int) count,
             (int) errno);
        quit_test(&context->base);
        return;
    }
    size_t len = strlen(*context->input);
    context->offset += count;
    if (context->offset < len) {
        async_execute(context->base.async,
                      (action_1) { context, (act_1) qstr_probe });
        return;
    }
    if (context->offset > len) {
        tlog("Too many bytes from queuestream");
        quit_test(&context->base);
        return;
    }
    if (memcmp(*context->input, context->buffer, len)) {
        tlog("Input mismatch");
        quit_test(&context->base);
        return;
    }
    context->offset = 0;
    context->input++;
    context->state = QSTR_ENQUEUE_INPUT;
    async_execute(context->base.async,
                  (action_1) { context, (act_1) qstr_probe });
}

static void qstr_terminated(tester_t *context, ssize_t count)
{
    if (count != 0) {
        tlog("Unexpected error %d (errno %d) from queuestream", (int) count,
             (int) errno);
        quit_test(&context->base);
        return;
    }
    context->state = QSTR_DONE;
    context->base.verdict = PASS;
    quit_test(&context->base);
}

static void qstr_probe(tester_t *context)
{
    if (context->state == QSTR_DONE) /* spurious? */
        return;
    ssize_t count =
        queuestream_read(context->qstr, context->buffer + context->offset,
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
    async_t *async = make_async();
    tester_t context = {
        .state = QSTR_ENQUEUE_INPUT,
        .input = INPUTS,
    };
    init_test(&context.base, async, 2);
    queuestream_t *qstr = context.qstr = make_queuestream(async);
    action_1 qstr_probe_cb = { &context, (act_1) qstr_probe };
    queuestream_register_callback(qstr, qstr_probe_cb);
    async_execute(async, qstr_probe_cb);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    queuestream_close(qstr);
    destroy_async(async);
    return posttest_check(context.base.verdict);
}

VERDICT test_relaxed_queuestream(void)
{
    async_t *async = make_async();
    tester_t context = {
        .state = QSTR_ENQUEUE_INPUT,
        .input = INPUTS,
    };
    init_test(&context.base, async, 2);
    queuestream_t *qstr = context.qstr = make_relaxed_queuestream(async);
    action_1 qstr_probe_cb = { &context, (act_1) qstr_probe };
    queuestream_register_callback(qstr, qstr_probe_cb);
    async_execute(async, qstr_probe_cb);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    if (context.base.verdict != PASS)
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
    return posttest_check(context.base.verdict);
}
