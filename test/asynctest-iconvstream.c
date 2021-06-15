#include "asynctest-iconvstream.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>

#include <async/iconvstream.h>
#include <async/pacerstream.h>
#include <async/queuestream.h>
#include <async/stringstream.h>

typedef struct {
    tester_base_t base;
    bytestream_1 output;
    size_t char_count;
} tester_t;

static const char *test_text = "Öisin kävellään töihin löhöilemään.\n";
enum {
    LATIN_9_LENGTH = 36,
    REPEAT_COUNT = 20000,
    TOTAL_OUTPUT = REPEAT_COUNT * LATIN_9_LENGTH
};

static void verify_read(tester_t *tester)
{
    if (!tester->base.async)
        return;
    char buffer[119];
    ssize_t count = bytestream_1_read(tester->output, buffer, sizeof buffer);
    if (count < 0) {
        if (errno != EAGAIN) {
            tlog("Errno %d from iconvstream_read", errno);
            quit_test(&tester->base);
        }
        return;
    }
    if (count == 0) {
        if (tester->char_count != TOTAL_OUTPUT)
            tlog("Final char_count %u != %u (expected)",
                 (unsigned) tester->char_count, (unsigned) TOTAL_OUTPUT);
        else
            tester->base.verdict = PASS;
        bytestream_1_close(tester->output);
        quit_test(&tester->base);
        return;
    }
    tester->char_count += count;
    action_1 verification_cb = { tester, (act_1) verify_read };
    async_execute(tester->base.async, verification_cb);
}

VERDICT test_iconvstream(void)
{
    async_t *async = make_async();
    queuestream_t *qstr = make_queuestream(async);
    unsigned i;
    for (i = 0; i < REPEAT_COUNT; i++) {
        stringstream_t *sstr = open_stringstream(async, test_text);
        queuestream_enqueue(qstr, stringstream_as_bytestream_1(sstr));
    }
    queuestream_terminate(qstr);
    pacerstream_t *pstr =
        pace_stream(async, queuestream_as_bytestream_1(qstr), 500000, 10, 200);
    iconvstream_t *icstr =
        open_iconvstream(async, pacerstream_as_bytestream_1(pstr),
                         "ISO-8859-15", "UTF-8");
    assert(icstr);
    tester_t tester = {
        .output = iconvstream_as_bytestream_1(icstr),
    };
    init_test(&tester.base, async, 10);
    action_1 verification_cb = { &tester, (act_1) verify_read };
    iconvstream_register_callback(icstr, verification_cb);
    async_execute(async, verification_cb);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    destroy_async(async);
    return posttest_check(tester.base.verdict);
}
