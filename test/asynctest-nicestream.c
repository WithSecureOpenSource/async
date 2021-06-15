#include "asynctest-nicestream.h"

#include <errno.h>

#include <async/async.h>
#include <async/nicestream.h>
#include <async/zerostream.h>

typedef struct {
    tester_base_t base;
    bytestream_1 stream;
    int count;
} tester_t;

static void nice_probe(tester_t *context)
{
    enum { REPEAT = 5 };
    uint8_t buffer[100];
    ssize_t count = bytestream_1_read(context->stream, buffer, sizeof buffer);
    if (count != sizeof buffer) {
        tlog("Expected a full buffer, got %d (errno = %d)", (int) count,
             (int) errno);
        quit_test(&context->base);
        return;
    }
    count = bytestream_1_read(context->stream, buffer, sizeof buffer);
    if (count >= 0 || errno != EAGAIN) {
        tlog("Expected EAGAIN, got %d (errno = %d)", (int) count, (int) errno);
        quit_test(&context->base);
        return;
    }
    if (++context->count == REPEAT) {
        context->base.verdict = PASS;
        quit_test(&context->base);
        return;
    }
}

VERDICT test_nicestream(void)
{
    enum { MAX_BURST = 10 };
    async_t *async = make_async();
    tester_t context = {
        .count = 0,
    };
    init_test(&context.base, async, 2);
    nicestream_t *nicestr = make_nice(async, zerostream, MAX_BURST);
    context.stream = nicestream_as_bytestream_1(nicestr);
    action_1 probe = { &context, (act_1) nice_probe };
    nicestream_register_callback(nicestr, probe);
    async_execute(async, probe);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    nicestream_close(nicestr);
    destroy_async(async);
    return posttest_check(context.base.verdict);
}
