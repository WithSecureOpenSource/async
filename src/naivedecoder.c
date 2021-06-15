#include "naivedecoder.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"

enum {
    NAIVEDECODER_READING,
    NAIVEDECODER_ESCAPED,
    NAIVEDECODER_TERMINATED, /* final for NAIVEDECODER_DETACH */
    NAIVEDECODER_EXHAUSTED,  /* final for NAIVEDECODER_ADOPT_INPUT */
    NAIVEDECODER_ERROR,
    NAIVEDECODER_CLOSED,
};

struct naivedecoder {
    async_t *async;
    uint64_t uid;
    bytestream_1 source;
    int mode, state;
    uint8_t buffer[5000];
    size_t low, high;
    uint8_t terminator, escape;
};

static ssize_t do_read(naivedecoder_t *decoder, void *buf, size_t size)
{
    switch (decoder->state) {
        case NAIVEDECODER_READING:
        case NAIVEDECODER_ESCAPED: {
            if (decoder->low >= decoder->high) {
                ssize_t more =
                    bytestream_1_read(decoder->source, decoder->buffer,
                                      sizeof decoder->buffer);
                if (more < 0)
                    return -1;
                if (!more) {
                    decoder->state = NAIVEDECODER_ERROR;
                    errno = EPROTO;
                    return -1;
                }
                decoder->low = 0;
                decoder->high = more;
            }
            uint8_t *p = buf;
            size_t n = 0;
            while (n < size && decoder->low < decoder->high) {
                uint8_t next_byte = decoder->buffer[decoder->low++];
                if (decoder->state == NAIVEDECODER_ESCAPED)
                    decoder->state = NAIVEDECODER_READING;
                else if (next_byte == decoder->terminator) {
                    decoder->state = NAIVEDECODER_TERMINATED;
                    break;
                } else if (next_byte == decoder->escape) {
                    decoder->state = NAIVEDECODER_ESCAPED;
                    continue;
                }
                *p++ = next_byte;
                n++;
            }
            if (!n && decoder->state != NAIVEDECODER_READING)
                return naivedecoder_read(decoder, buf, size);
            return n;
        }
        case NAIVEDECODER_TERMINATED:
            if (decoder->mode == NAIVEDECODER_DETACH)
                return 0;
            switch (bytestream_1_read(decoder->source, decoder->buffer, 1)) {
                case 0:
                    decoder->state = NAIVEDECODER_EXHAUSTED;
                    return 0;
                case 1:
                    decoder->state = NAIVEDECODER_ERROR;
                    errno = EPROTO;
                    return -1;
                default:
                    return -1;
            }
        case NAIVEDECODER_EXHAUSTED:
            return 0;
        case NAIVEDECODER_ERROR:
            errno = EPROTO;
            return -1;
        default:
            assert(false);
    }
}

FSTRACE_DECL(ASYNC_NAIVEDECODER_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_NAIVEDECODER_READ_DUMP, "UID=%64u DATA=%B");

ssize_t naivedecoder_read(naivedecoder_t *decoder, void *buf, size_t size)
{
    ssize_t count = do_read(decoder, buf, size);
    FSTRACE(ASYNC_NAIVEDECODER_READ, decoder->uid, size, count);
    FSTRACE(ASYNC_NAIVEDECODER_READ_DUMP, decoder->uid, buf, count);
    return count;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return naivedecoder_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_NAIVEDECODER_CLOSE, "UID=%64u");

void naivedecoder_close(naivedecoder_t *decoder)
{
    FSTRACE(ASYNC_NAIVEDECODER_CLOSE, decoder->uid);
    assert(decoder->state != NAIVEDECODER_CLOSED);
    if (decoder->mode == NAIVEDECODER_ADOPT_INPUT)
        bytestream_1_close(decoder->source);
    async_wound(decoder->async, decoder);
    decoder->state = NAIVEDECODER_CLOSED;
}

static void _close(void *obj)
{
    naivedecoder_close(obj);
}

FSTRACE_DECL(ASYNC_NAIVEDECODER_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void naivedecoder_register_callback(naivedecoder_t *decoder, action_1 action)
{
    FSTRACE(ASYNC_NAIVEDECODER_REGISTER, decoder->uid, action.obj, action.act);
    bytestream_1_register_callback(decoder->source, action);
}

static void _register_callback(void *obj, action_1 action)
{
    naivedecoder_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_NAIVEDECODER_UNREGISTER, "UID=%64u");

void naivedecoder_unregister_callback(naivedecoder_t *decoder)
{
    FSTRACE(ASYNC_NAIVEDECODER_UNREGISTER, decoder->uid);
    bytestream_1_unregister_callback(decoder->source);
}

static void _unregister_callback(void *obj)
{
    naivedecoder_unregister_callback(obj);
}

static ssize_t _remaining(void *obj)
{
    errno = ENOTSUP;
    return -1;
}

size_t naivedecoder_leftover_size(naivedecoder_t *decoder)
{
    return decoder->high - decoder->low;
}

static ssize_t _leftover_size(void *obj)
{
    return naivedecoder_leftover_size(obj);
}

void *naivedecoder_leftover_bytes(naivedecoder_t *decoder)
{
    return decoder->buffer + decoder->low;
}

static void *_leftover_bytes(void *obj)
{
    return naivedecoder_leftover_bytes(obj);
}

static const struct bytestream_2_vt naivedecoder_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
    .remaining = _remaining,
    .leftover_size = _leftover_size,
    .leftover_bytes = _leftover_bytes,
};

bytestream_2 naivedecoder_as_bytestream_2(naivedecoder_t *decoder)
{
    return (bytestream_2) { decoder, &naivedecoder_vt };
}

bytestream_1 naivedecoder_as_bytestream_1(naivedecoder_t *decoder)
{
    bytestream_2 stream = naivedecoder_as_bytestream_2(decoder);
    return bytestream_2_as_bytestream_1(stream);
}

static const char *trace_mode(void *pmode)
{
    switch (*(int *) pmode) {
        case NAIVEDECODER_DETACH:
            return "NAIVEDECODER_DETACH";
        case NAIVEDECODER_ADOPT_INPUT:
            return "NAIVEDECODER_ADOPT_INPUT";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNC_NAIVEDECODER_CREATE,
             "UID=%64u PTR=%p ASYNC=%p SOURCE=%p MODE=%I");

naivedecoder_t *naive_decode(async_t *async, bytestream_1 source, int mode,
                             uint8_t terminator, uint8_t escape)
{
    naivedecoder_t *decoder = fsalloc(sizeof *decoder);
    decoder->async = async;
    decoder->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_NAIVEDECODER_CREATE, decoder->uid, decoder, async, source.obj,
            trace_mode, &mode);
    decoder->source = source;
    decoder->mode = mode;
    decoder->state = NAIVEDECODER_READING;
    decoder->terminator = terminator;
    decoder->escape = escape;
    decoder->low = decoder->high = 0;
    return decoder;
}
