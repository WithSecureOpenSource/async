#include <errno.h>
#include <async/async.h>
#include <async/drystream.h>
#include "asynctest-drystream.h"

typedef struct {
    async_t *async;
    bytestream_1 stream;
    int count;
    int verdict;
} DRY_CONTEXT;

static void dry_probe(DRY_CONTEXT *context)
{
    uint8_t buffer[100];
    ssize_t count = bytestream_1_read(drystream, buffer, sizeof buffer);
    if (count >= 0 || errno != EAGAIN)
        async_quit_loop(context->async);
}

static void dry_timeout(DRY_CONTEXT *context)
{
    context->verdict = PASS;
    action_1 quit = { context->async, (act_1) async_quit_loop };
    async_execute(context->async, quit);
}

VERDICT test_drystream(void)
{
    DRY_CONTEXT context = {
        .verdict = FAIL
    };
    async_t *async = context.async = make_async();
    action_1 probe = { &context, (act_1) dry_probe };
    bytestream_1_register_callback(drystream, probe);
    async_execute(async, probe);
    async_timer_start(async, async_now(async) + 2 * ASYNC_S,
                      (action_1) { &context, (act_1) dry_timeout });
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    bytestream_1_close(drystream);
    destroy_async(async);
    uint8_t buffer[100];
    ssize_t count = bytestream_1_read(drystream, buffer, sizeof buffer);
    if (count >= 0 || errno != EAGAIN)
        return FAIL;
    return posttest_check(context.verdict);
}
