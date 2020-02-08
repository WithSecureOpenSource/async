#include <errno.h>
#include <string.h>
#include <assert.h>
#include <async/jsonyield.h>
#include <async/jsonencoder.h>
#include <async/queuestream.h>
#include <async/pacerstream.h>
#include <async/naiveencoder.h>
#include "asynctest-framers.h"

typedef struct {
    async_t *async;
    yield_1 yield;
    async_timer_t *timer;
    size_t pdu_count;
    VERDICT verdict;
} tester_t;

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

static void verify_receive(tester_t *tester)
{
    if (!tester->async)
        return;
    json_thing_t *data = yield_1_receive(tester->yield);
    if (data) {
        if (json_thing_type(data) != JSON_OBJECT) {
            quit_test(tester);
            return;
        }
        json_thing_t *sweden = json_object_get(data, "Sweden");
        if (!sweden) {
            quit_test(tester);
            return;
        }
        json_thing_t *population = json_object_get(sweden, "population");
        if (!population) {
            quit_test(tester);
            return;
        }
        unsigned long long n;
        if (!json_cast_to_unsigned(population, &n) || n != 9900000) {
            quit_test(tester);
            return;
        }
        json_destroy_thing(data);
        action_1 verification_cb = { tester, (act_1) verify_receive };
        async_execute(tester->async, verification_cb);
        tester->pdu_count++;
        return;
    }
    switch (errno) {
        case EAGAIN:
            break;
        case 0:
            if (tester->pdu_count != 200)
                tlog("Final pdu_count %u != %u (expected)",
                     (unsigned) tester->pdu_count, (unsigned) 200);
            else tester->verdict = PASS;
            yield_1_close(tester->yield);
            quit_test(tester);
            break;
        default:
            tlog("Errno %d from jsonyield_receive", errno);
            quit_test(tester);
    }
}

static json_thing_t *make_test_data(void)
{
    json_thing_t *data = json_make_object();
    json_thing_t *finland = json_make_object();
    json_add_to_object(finland, "capital", json_make_string("Helsinki"));
    json_add_to_object(finland, "population", json_make_unsigned(5500000));
    json_add_to_object(data, "Finland", finland);
    json_thing_t *sweden = json_make_object();
    json_add_to_object(sweden, "capital", json_make_string("Stockholm"));
    json_add_to_object(sweden, "population", json_make_unsigned(9900000));
    json_add_to_object(data, "Sweden", sweden);
    return data;
}

VERDICT test_json(void)
{
    async_t *async = make_async();
    queuestream_t *qstr = make_queuestream(async);
    unsigned i;
    json_thing_t *data = make_test_data();
    for (i = 0; i < 200; i++) {
        bytestream_1 payload =
            jsonencoder_as_bytestream_1(json_encode(async, data));
        naiveencoder_t *naive_encoder =
            naive_encode(async, payload, '\0', '\33');
        queuestream_enqueue(qstr,
                            naiveencoder_as_bytestream_1(naive_encoder));
    }
    json_destroy_thing(data);
    queuestream_terminate(qstr);
    pacerstream_t *pstr = pace_stream(async,
                                      queuestream_as_bytestream_1(qstr),
                                      5000, 10, 200);
    jsonyield_t *yield = open_jsonyield(async,
                                        pacerstream_as_bytestream_1(pstr),
                                        300);
    tester_t tester = {
        .async = async,
        .yield = jsonyield_as_yield_1(yield),
        .verdict = FAIL,
    };
    action_1 timeout_cb = { &tester, (act_1) test_timeout };
    enum { MAX_DURATION = 10 };
    tlog("  max duration = %d s", MAX_DURATION);
    tester.timer =
        async_timer_start(async, async_now(async) + MAX_DURATION * ASYNC_S,
                          timeout_cb);
    action_1 verification_cb = { &tester, (act_1) verify_receive };
    jsonyield_register_callback(yield, verification_cb);
    async_execute(async, verification_cb);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    destroy_async(async);
    return posttest_check(tester.verdict);
}
