#include "asynctest-framers.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <async/blobstream.h>
#include <async/chunkencoder.h>
#include <async/chunkframer.h>
#include <async/naiveencoder.h>
#include <async/naiveframer.h>
#include <async/pacerstream.h>
#include <async/queuestream.h>
#include <fsdyn/fsalloc.h>

typedef struct {
    async_t *async;
    size_t cursor, size;
} primesource_t;

static ssize_t _read(void *obj, void *buf, size_t count)
{
    primesource_t *source = obj;
    uint8_t *p = buf;
    size_t n;
    for (n = 0; count-- && source->cursor < source->size; n++)
        *p++ = source->cursor++ % 31;
    return n;
}

static void _close(void *obj)
{
    primesource_t *source = obj;
    async_wound(source->async, source);
    source->async = NULL;
}

static void _register_callback(void *obj, action_1 action) {}

static void _unregister_callback(void *obj) {}

static struct bytestream_1_vt primesource_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

static primesource_t *open_primesource(async_t *async, size_t size)
{
    primesource_t *source = fsalloc(sizeof *source);
    source->async = async;
    source->cursor = 0;
    source->size = size;
    return source;
}

static bytestream_1 primesource_as_bytestream_1(primesource_t *source)
{
    return (bytestream_1) { source, &primesource_vt };
}

static bytestream_1 make_payload(async_t *async, size_t size)
{
    return primesource_as_bytestream_1(open_primesource(async, size));
}

static void enq_chunked_pdu(async_t *async, queuestream_t *qstr,
                            bytestream_1 payload)
{
    chunkencoder_t *encoder = chunk_encode(async, payload, 501);
    queuestream_enqueue(qstr, chunkencoder_as_bytestream_1(encoder));
}

static void enq_naive_pdu(async_t *async, queuestream_t *qstr,
                          bytestream_1 payload)
{
    naiveencoder_t *encoder = naive_encode(async, payload, -1, -1);
    queuestream_enqueue(qstr, naiveencoder_as_bytestream_1(encoder));
}

typedef struct {
    tester_base_t base;
    yield_1 framer;
    bytestream_1 *pdu;
    size_t pdu_count;
    size_t cursor;
} tester_t;

static void verify_receive(tester_t *tester);

static void verify_read(tester_t *tester)
{
    if (!tester->base.async || !tester->pdu)
        return;
    uint8_t buffer[1000];
    ssize_t count = bytestream_1_read(*tester->pdu, buffer, sizeof buffer);
    if (count < 0) {
        if (errno == EAGAIN)
            return;
        tlog("Errno %d from PDU read", errno);
        quit_test(&tester->base);
        return;
    }
    if (!count) {
        size_t expected;
        switch (tester->pdu_count % 3) {
            case 0:
                expected = 63;
                break;
            case 1:
                expected = 6300;
                break;
            default:
                expected = 630001;
        }
        if (tester->cursor != expected) {
            tlog("Unexpected PDU[%u] size %u != %u (expected)",
                 (unsigned) tester->pdu_count, (unsigned) tester->cursor,
                 (unsigned) expected);
            quit_test(&tester->base);
            return;
        }
        bytestream_1_close(*tester->pdu);
        tester->pdu_count++;
        tester->cursor = 0;
        tester->pdu = NULL;
        verify_receive(tester);
        return;
    }
    size_t i = 0;
    while (i < count)
        if (buffer[i++] != tester->cursor++ % 31) {
            tlog("Unexpected PDU[%u] content", (unsigned) tester->pdu_count);
            quit_test(&tester->base);
            return;
        }
    action_1 verification_cb = { tester, (act_1) verify_read };
    async_execute(tester->base.async, verification_cb);
}

static void verify_receive(tester_t *tester)
{
    if (!tester->base.async || tester->pdu)
        return;
    tester->pdu = yield_1_receive(tester->framer);
    if (tester->pdu) {
        tester->cursor = 0;
        action_1 verification_cb = { tester, (act_1) verify_read };
        bytestream_1_register_callback(*tester->pdu, verification_cb);
        async_execute(tester->base.async, verification_cb);
        return;
    }
    switch (errno) {
        case EAGAIN:
            break;
        case 0:
            if (tester->pdu_count != 200 * 3)
                tlog("Final pdu_count %u != %u (expected)",
                     (unsigned) tester->pdu_count, (unsigned) 200 * 3);
            else
                tester->base.verdict = PASS;
            yield_1_close(tester->framer);
            quit_test(&tester->base);
            break;
        default:
            tlog("Errno %d from chunkframer_receive", errno);
            quit_test(&tester->base);
    }
}

static yield_1 open_chunk_yield(async_t *async, bytestream_1 source)
{
    return chunkframer_as_yield_1(open_chunkframer(async, source));
}

static yield_1 open_naive_yield(async_t *async, bytestream_1 source)
{
    return naiveframer_as_yield_1(open_naiveframer(async, source, -1, -2));
}

static VERDICT test_framer(void (*enq_pdu)(async_t *, queuestream_t *,
                                           bytestream_1),
                           yield_1 (*open_framer)(async_t *, bytestream_1))
{
    async_t *async = make_async();
    queuestream_t *qstr = make_queuestream(async);
    unsigned i;
    for (i = 0; i < 200; i++) {
        enq_pdu(async, qstr, make_payload(async, 63));
        enq_pdu(async, qstr, make_payload(async, 6300));
        enq_pdu(async, qstr, make_payload(async, 630001));
    }
    queuestream_terminate(qstr);
    pacerstream_t *pstr = pace_stream(async, queuestream_as_bytestream_1(qstr),
                                      5000000, 101, 101010);
    yield_1 framer = open_framer(async, pacerstream_as_bytestream_1(pstr));
    tester_t tester = {
        .framer = framer,
    };
    init_test(&tester.base, async, 30);
    action_1 verification_cb = { &tester, (act_1) verify_receive };
    yield_1_register_callback(framer, verification_cb);
    async_execute(async, verification_cb);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    destroy_async(async);
    return posttest_check(tester.base.verdict);
}

VERDICT test_chunkframer(void)
{
    return test_framer(enq_chunked_pdu, open_chunk_yield);
}

VERDICT test_naiveframer(void)
{
    return test_framer(enq_naive_pdu, open_naive_yield);
}
