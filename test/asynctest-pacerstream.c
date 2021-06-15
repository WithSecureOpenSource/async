#include "asynctest-pacerstream.h"

#include <errno.h>

#include <async/async.h>
#include <async/pacerstream.h>
#include <async/substream.h>
#include <async/zerostream.h>

typedef struct {
    tester_base_t base;
    pacerstream_t *pacer;
} tester_t;

enum {
    TOTAL_BYTE_COUNT = 100000,
    MIN_BURST = TOTAL_BYTE_COUNT / 100,
    MAX_BURST = TOTAL_BYTE_COUNT / 10
};

#define TOTAL_TIME 2.0
static const double PACE = TOTAL_BYTE_COUNT / TOTAL_TIME;

static void probe(tester_t *context)
{
    if (!context->base.async)
        return;

    uint8_t buffer[100];
    size_t burst_size = 0;
    for (;;) {
        ssize_t count = pacerstream_read(context->pacer, buffer, sizeof buffer);
        if (count < 0) {
            if (errno != EAGAIN) {
                tlog("Unexpected error %d (errno %d)", (int) count,
                     (int) errno);
                quit_test(&context->base);
                return;
            }
            return;
        }
        if (count == 0) {
            context->base.verdict = PASS;
            quit_test(&context->base);
            return;
        }
        burst_size += count;
        if (burst_size > MAX_BURST) {
            tlog("Maximum burst size exceeded");
            quit_test(&context->base);
            return;
        }
    }
}

VERDICT test_pacerstream(void)
{
    async_t *async = make_async();
    tester_t context = {};
    init_test(&context.base, async, TOTAL_TIME + 2);
    substream_t *substr =
        make_substream(async, zerostream, SUBSTREAM_CLOSE_AT_END, 0,
                       TOTAL_BYTE_COUNT);
    pacerstream_t *pacer = context.pacer =
        pace_stream(async, substream_as_bytestream_1(substr), PACE, MIN_BURST,
                    MAX_BURST);
    action_1 probe_action = (action_1) { &context, (act_1) probe };
    pacerstream_register_callback(pacer, probe_action);
    async_execute(async, probe_action);
    uint64_t t0 = async_now(async);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    if (context.base.verdict != PASS)
        return context.base.verdict;
    uint64_t t1 = async_now(async);
    double duration = (t1 - t0) / (double) ASYNC_S;
    if (duration < 0.9 * TOTAL_TIME) {
        tlog("Too fast: expected %f s -- was %f s", TOTAL_TIME, duration);
        return FAIL;
    }
    if (duration > 1.1 * TOTAL_TIME) {
        tlog("Too slow: expected %f s -- was %f s", TOTAL_TIME, duration);
        return FAIL;
    }
    pacerstream_close(pacer);
    destroy_async(async);
    return posttest_check(PASS);
}
