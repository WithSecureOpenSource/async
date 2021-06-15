#include "naiveencoder.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <fsdyn/fsalloc.h>

#include "async.h"
#include "async_version.h"

enum {
    NAIVEENCODER_READING,
    NAIVEENCODER_ESCAPED,    /* escape byte delivered */
    NAIVEENCODER_EXHAUSTED,  /* source exhausted; terminator pending */
    NAIVEENCODER_TERMINATED, /* terminator delivered */
    NAIVEENCODER_ERROR,
    NAIVEENCODER_CLOSED
};

struct naiveencoder {
    async_t *async;
    bytestream_1 source;
    int state;
    uint8_t terminator, escape;
    uint8_t buffer[2000];
    size_t low, high;
};

ssize_t naiveencoder_read(naiveencoder_t *encoder, void *buf, size_t count)
{
    switch (encoder->state) {
        case NAIVEENCODER_READING:
        case NAIVEENCODER_ESCAPED: {
            if (encoder->low >= encoder->high) {
                ssize_t more =
                    bytestream_1_read(encoder->source, encoder->buffer,
                                      sizeof encoder->buffer);
                if (more < 0)
                    return -1;
                if (!more) {
                    encoder->state = NAIVEENCODER_EXHAUSTED;
                    return naiveencoder_read(encoder, buf, count);
                }
                encoder->low = 0;
                encoder->high = more;
            }
            uint8_t *p = buf;
            size_t n = 0;
            while (n < count && encoder->low < encoder->high) {
                uint8_t next_byte = encoder->buffer[encoder->low++];
                if (encoder->state == NAIVEENCODER_ESCAPED) {
                    encoder->state = NAIVEENCODER_READING;
                    *p++ = next_byte;
                } else if (next_byte == encoder->terminator ||
                           next_byte == encoder->escape) {
                    if (encoder->terminator == encoder->escape) {
                        encoder->state = NAIVEENCODER_ERROR;
                        errno = EPROTO;
                        return -1;
                    }
                    encoder->state = NAIVEENCODER_ESCAPED;
                    *p++ = encoder->escape;
                } else {
                    *p++ = next_byte;
                }
                n++;
            }
            return n;
        }
        case NAIVEENCODER_EXHAUSTED:
            if (!count)
                return 0;
            *(uint8_t *) buf = encoder->terminator;
            encoder->state = NAIVEENCODER_TERMINATED;
            return 1;
        case NAIVEENCODER_TERMINATED:
            return 0;
        case NAIVEENCODER_ERROR:
            errno = EPROTO;
            return -1;
        default:
            errno = EBADF;
            return -1;
    }
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return naiveencoder_read(obj, buf, count);
}

void naiveencoder_close(naiveencoder_t *encoder)
{
    assert(encoder->state != NAIVEENCODER_CLOSED);
    bytestream_1_close(encoder->source);
    async_wound(encoder->async, encoder);
    encoder->state = NAIVEENCODER_CLOSED;
}

static void _close(void *obj)
{
    naiveencoder_close(obj);
}

void naiveencoder_register_callback(naiveencoder_t *encoder, action_1 action)
{
    bytestream_1_register_callback(encoder->source, action);
}

static void _register_callback(void *obj, action_1 action)
{
    naiveencoder_register_callback(obj, action);
}

void naiveencoder_unregister_callback(naiveencoder_t *encoder)
{
    bytestream_1_unregister_callback(encoder->source);
}

static void _unregister_callback(void *obj)
{
    naiveencoder_unregister_callback(obj);
}

static const struct bytestream_1_vt naiveencoder_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 naiveencoder_as_bytestream_1(naiveencoder_t *encoder)
{
    return (bytestream_1) { encoder, &naiveencoder_vt };
}

naiveencoder_t *naive_encode(async_t *async, bytestream_1 source,
                             uint8_t terminator, uint8_t escape)
{
    naiveencoder_t *encoder = fsalloc(sizeof *encoder);
    encoder->async = async;
    encoder->source = source;
    encoder->state = NAIVEENCODER_READING;
    encoder->terminator = terminator;
    encoder->escape = escape;
    encoder->low = encoder->high = 0;
    return encoder;
}
