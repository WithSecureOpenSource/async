#include <errno.h>
#include <stddef.h>
#include <assert.h>
#include <async/queuestream.h>
#include <async/stringstream.h>
#include <async/pacerstream.h>
#include <async/iconvstream.h>
#include "asynctest-iconvstream.h"

typedef struct {
    async_t *async;
    bytestream_1 output;
    async_timer_t *timer;
    size_t char_count;
    VERDICT verdict;
} tester_t;

static const char *test_text = "Öisin kävellään töihin löhöilemään.\n";
enum {
    LATIN_9_LENGTH = 36,
    REPEAT_COUNT = 20000,
    TOTAL_OUTPUT = REPEAT_COUNT * LATIN_9_LENGTH
};

static void do_quit(tester_t *tester)
{
    action_1 quitter = { tester->async, (act_1) async_quit_loop };
    tester->timer = NULL;
    async_execute(tester->async, quitter);
    tester->async = NULL;
}

static void quit_test(tester_t *tester)
{
    async_timer_cancel(tester->async, tester->timer);
    do_quit(tester);
}

static void test_timeout(tester_t *tester)
{
    tlog("Test timeout");
    do_quit(tester);
}

static void verify_read(tester_t *tester)
{
    if (!tester->async)
        return;
    char buffer[119];
    ssize_t count = bytestream_1_read(tester->output, buffer, sizeof buffer);
    if (count < 0) {
        if (errno != EAGAIN) {
            tlog("Errno %d from iconvstream_read", errno);
            quit_test(tester);
        }
        return;
    }
    if (count == 0) {
        if (tester->char_count != TOTAL_OUTPUT)
            tlog("Final char_count %u != %u (expected)",
                 (unsigned) tester->char_count, (unsigned) TOTAL_OUTPUT);
        else tester->verdict = PASS;
        bytestream_1_close(tester->output);
        quit_test(tester);
        return;
    }
    tester->char_count += count;
    action_1 verification_cb = { tester, (act_1) verify_read };
    async_execute(tester->async, verification_cb);
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
    pacerstream_t *pstr = pace_stream(async,
                                      queuestream_as_bytestream_1(qstr),
                                      500000, 10, 200);
    iconvstream_t *icstr =
        open_iconvstream(async, pacerstream_as_bytestream_1(pstr),
                         "LATIN-9", "UTF-8");
    assert(icstr);
    tester_t tester = {
        .async = async,
        .output = iconvstream_as_bytestream_1(icstr),
        .verdict = FAIL,
    };
    action_1 timeout_cb = { &tester, (act_1) test_timeout };
    enum { MAX_DURATION = 10 };
    tlog("  max duration = %d s", MAX_DURATION);
    tester.timer =
        async_timer_start(async, async_now(async) + MAX_DURATION * ASYNC_S,
                          timeout_cb);
    action_1 verification_cb = { &tester, (act_1) verify_read };
    iconvstream_register_callback(icstr, verification_cb);
    async_execute(async, verification_cb);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    destroy_async(async);
    return posttest_check(tester.verdict);
}
