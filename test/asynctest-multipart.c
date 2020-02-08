#include "asynctest-multipart.h"

#include <async/multipartdeserializer.h>
#include <async/queuestream.h>
#include <fsdyn/bytearray.h>
#include <fsdyn/fsalloc.h>

#include <assert.h>
#include <errno.h>
#include <string.h>

typedef struct {
    async_t *async;
    yield_1 yield;
    bytestream_1 *part;
    byte_array_t *buffer;
    async_timer_t *timer;
    size_t pdu_count;
    char **parts;
    size_t parts_size;
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

static bool verify_part(byte_array_t *array, const char *part)
{
    bool match = true;
    size_t len = byte_array_size(array);
    const char *buffer = byte_array_data(array);
    if (len != strlen(part) || memcmp(buffer, part, len)) {
        tlog("expected: '%s' received: '%s'", part, buffer);
        match = false;
    }
    return match;
}

static ssize_t read_part(void *obj, void *buf, size_t count)
{
    bytestream_1 *stream = obj;
    return bytestream_1_read(*stream, buf, count);
}

static void verify_receive(tester_t *tester);

static void verify_read(tester_t *tester)
{
    if (!tester->async)
        return;
    ssize_t count =
        byte_array_append_stream(tester->buffer, read_part, tester->part, 1024);
    if (count < 0) {
        if (errno != EAGAIN) {
            tlog("Errno %d from bytestream_1_read", errno);
            quit_test(tester);
        }
        return;
    }
    if (!count) {
        if (tester->pdu_count < tester->parts_size &&
            !verify_part(tester->buffer, tester->parts[tester->pdu_count])) {
            quit_test(tester);
            return;
        }
        tester->pdu_count++;
        bytestream_1_close(*tester->part);
        verify_receive(tester);
        return;
    }
    action_1 verification_cb = { tester, (act_1) verify_read };
    async_execute(tester->async, verification_cb);
}

static void verify_receive(tester_t *tester)
{
    if (!tester->async)
        return;
    tester->part = yield_1_receive(tester->yield);
    if (!tester->part) {
        if (errno == EAGAIN)
            return;
        if (!errno) {
            if (tester->pdu_count != tester->parts_size)
                tlog("expected %u parts received %u parts",
                     (unsigned) tester->pdu_count,
                     (unsigned) tester->parts_size);
            else
                tester->verdict = PASS;
        } else
            tlog("Errno %d from yield_1_receive", errno);
        yield_1_close(tester->yield);
        quit_test(tester);
        return;
    }
    byte_array_clear(tester->buffer);
    action_1 verification_cb = { tester, (act_1) verify_read };
    bytestream_1_register_callback(*tester->part, verification_cb);
    async_execute(tester->async, verification_cb);
}

typedef struct {
    char *input;
    char *boundary;
    char **parts;
    size_t parts_size;
} test_data_t;

VERDICT test_multipart_single(test_data_t *test_data)
{
    async_t *async = make_async();
    queuestream_t *qstr = make_queuestream(async);
    queuestream_enqueue_bytes(qstr, test_data->input, strlen(test_data->input));
    queuestream_terminate(qstr);
    multipartdeserializer_t *des =
        open_multipartdeserializer(async,
                                   queuestream_as_bytestream_1(qstr),
                                   test_data->boundary);
    tester_t tester = {
        .async = async,
        .yield = multipartdeserializer_as_yield_1(des),
        .buffer = make_byte_array(1024),
        .verdict = FAIL,
        .parts = test_data->parts,
        .parts_size = test_data->parts_size,
    };
    action_1 timeout_cb = { &tester, (act_1) test_timeout };
    enum { MAX_DURATION = 10 };
    tlog("  max duration = %d s", MAX_DURATION);
    tester.timer = async_timer_start(async,
                                     async_now(async) + MAX_DURATION * ASYNC_S,
                                     timeout_cb);
    action_1 verification_cb = { &tester, (act_1) verify_receive };
    multipartdeserializer_register_callback(des, verification_cb);
    async_execute(async, verification_cb);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    destroy_byte_array(tester.buffer);
    destroy_async(async);
    return posttest_check(tester.verdict);
}

static test_data_t test_data[] = {
    { "--foo \t\r\n"
      "first part"
      "\r\n--foo  \r\n"
      "second part"
      "\r\n--foo--  \r\n",
      "foo",
      (char *[]) {
          "first part",
          "second part",
      },
      2 },
    { "--foo \t\r\n"
      "first part\r\n"
      "\r\n--foo  \r\n"
      "second part\r\n--fo"
      "\r\n--foo--  \r\n",
      "foo",
      (char *[]) {
          "first part\r\n",
          "second part\r\n--fo",
      },
      2 },
};

VERDICT test_multipart(void)
{
    int i;
    for (i = 0; i < sizeof(test_data) / sizeof(test_data[0]); i++) {
        VERDICT verdict = test_multipart_single(&test_data[i]);
        if (verdict == FAIL)
            return FAIL;
    }
    return PASS;
}
