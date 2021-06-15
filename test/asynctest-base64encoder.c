#include "asynctest-base64encoder.h"

#include <assert.h>
#include <errno.h>

#include <async/base64decoder.h>
#include <async/base64encoder.h>
#include <async/nicestream.h>
#include <fsdyn/fsalloc.h>

typedef struct {
    async_t *async;
    size_t size, cursor;
} source_t;

static source_t *open_source(async_t *async, size_t size)
{
    source_t *source = fsalloc(sizeof *source);
    source->async = async;
    source->size = size;
    source->cursor = 0;
    return source;
}

static ssize_t source_read(source_t *source, void *buf, size_t count)
{
    size_t remaining = source->size - source->cursor;
    if (remaining < count)
        count = remaining;
    size_t n;
    uint8_t *p = buf;
    for (n = 0; n < count; n++)
        *p++ = source->cursor++;
    return n;
}

static void source_close(source_t *source)
{
    assert(source->async);
    async_wound(source->async, source);
    source->async = NULL;
}

static void source_register_callback(void *obj, action_1 action) {}

static void source_unregister_callback(void *obj) {}

static ssize_t _source_read(void *obj, void *buf, size_t count)
{
    return source_read(obj, buf, count);
}

static void _source_close(void *obj)
{
    source_close(obj);
}

static void _source_register_callback(void *obj, action_1 action)
{
    source_register_callback(obj, action);
}

static void _source_unregister_callback(void *obj)
{
    source_unregister_callback(obj);
}

static struct bytestream_1_vt source_vt = {
    .read = _source_read,
    .close = _source_close,
    .register_callback = _source_register_callback,
    .unregister_callback = _source_unregister_callback,
};

static bytestream_1 source_as_bytestream_1(source_t *source)
{
    return (bytestream_1) { source, &source_vt };
}

typedef struct {
    tester_base_t base;
    bytestream_1 material;
    size_t size, cursor;
} tester_t;

static void verify_read(tester_t *tester)
{
    if (!tester->base.async)
        return;
    uint8_t buffer[200];
    ssize_t count = bytestream_1_read(tester->material, buffer, sizeof buffer);
    if (count < 0) {
        if (errno != EAGAIN) {
            tlog("Errno %d from bytestream_1_read", errno);
            quit_test(&tester->base);
        }
        return;
    }
    if (count == 0) {
        if (tester->cursor != tester->size)
            tlog("Final byte count %lu != %lu (expected)",
                 (unsigned long) tester->cursor, (unsigned long) tester->size);
        else
            tester->base.verdict = PASS;
        bytestream_1_close(tester->material);
        quit_test(&tester->base);
        return;
    }
    size_t i;
    for (i = 0; i < count; i++)
        if (buffer[i] != (uint8_t) tester->cursor++) {
            tester->cursor--;
            tlog("Bad byte at %lu: %u != %u (expected)",
                 (unsigned long) tester->cursor, buffer[i],
                 (uint8_t) tester->cursor);
            quit_test(&tester->base);
            return;
        }
    action_1 verification_cb = { tester, (act_1) verify_read };
    async_execute(tester->base.async, verification_cb);
}

VERDICT test_base64encoder(void)
{
    async_t *async = make_async();
    const size_t STREAM_LENGTH = 1000001;
    source_t *source = open_source(async, STREAM_LENGTH);
    nicestream_t *nicestr1 =
        make_nice(async, source_as_bytestream_1(source), 113);
    base64encoder_t *encoder =
        base64_encode(async, nicestream_as_bytestream_1(nicestr1), '.', '_',
                      true, '-');
    nicestream_t *nicestr2 =
        make_nice(async, base64encoder_as_bytestream_1(encoder), 91);
    base64decoder_t *decoder =
        base64_decode(async, nicestream_as_bytestream_1(nicestr2), '.', '_');
    nicestream_t *nicestr3 =
        make_nice(async, base64decoder_as_bytestream_1(decoder), 97);
    tester_t tester = {
        .material = nicestream_as_bytestream_1(nicestr3),
        .size = STREAM_LENGTH,
    };
    init_test(&tester.base, async, 2);
    action_1 verification_cb = { &tester, (act_1) verify_read };
    nicestream_register_callback(nicestr3, verification_cb);
    async_execute(async, verification_cb);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    destroy_async(async);
    return posttest_check(tester.base.verdict);
}
