#include <errno.h>
#include <async/async.h>
#include <async/nicestream.h>
#include <async/zerostream.h>
#include "asynctest-nicestream.h"

typedef struct {
    async_t *async;
    bytestream_1 stream;
    int count;
    int verdict;
} NICE_CONTEXT;

static void nice_probe(NICE_CONTEXT *context)
{
    enum { REPEAT = 5 };
    uint8_t buffer[100];
    ssize_t count = bytestream_1_read(context->stream, buffer, sizeof buffer);
    if (count != sizeof buffer) {
        tlog("Expected a full buffer, got %d (errno = %d)",
             (int) count, (int) errno);
        async_quit_loop(context->async);
        return;
    }
    count = bytestream_1_read(context->stream, buffer, sizeof buffer);
    if (count >= 0 || errno != EAGAIN) {
        tlog("Expected EAGAIN, got %d (errno = %d)",
             (int) count, (int) errno);
        async_quit_loop(context->async);
        return;
    }
    if (++context->count == REPEAT) {
        context->verdict = PASS;
        async_quit_loop(context->async);
        return;
    }
}

VERDICT test_nicestream(void)
{
    enum { MAX_BURST = 10 };
    NICE_CONTEXT context = {
        .count = 0,
        .verdict = FAIL
    };
    async_t *async = context.async = make_async();
    nicestream_t *nicestr = make_nice(async, zerostream, MAX_BURST);
    context.stream = nicestream_as_bytestream_1(nicestr);
    action_1 probe = { &context, (act_1) nice_probe };
    nicestream_register_callback(nicestr, probe);
    async_execute(async, probe);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    nicestream_close(nicestr);
    destroy_async(async);
    return posttest_check(context.verdict);
}
