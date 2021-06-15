#include "asynctest-drystream.h"

#include <errno.h>

#include <async/async.h>
#include <async/drystream.h>

static void dry_probe(tester_base_t *context)
{
    uint8_t buffer[100];
    ssize_t count = bytestream_1_read(drystream, buffer, sizeof buffer);
    if (count >= 0 || errno != EAGAIN) {
        context->verdict = FAIL;
        quit_test(context);
    }
}

VERDICT test_drystream(void)
{
    async_t *async = make_async();
    tester_base_t context;
    init_test(&context, async, 2);
    context.verdict = PASS;
    action_1 probe = { &context, (act_1) dry_probe };
    bytestream_1_register_callback(drystream, probe);
    async_execute(async, probe);
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
