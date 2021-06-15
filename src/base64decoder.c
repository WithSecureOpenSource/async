#include "base64decoder.h"

#include <assert.h>

#include <fsdyn/base64.h>
#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"

struct base64decoder {
    async_t *async;
    uint64_t uid;
    bytestream_1 stream;
    char pos62, pos63;
    size_t bit_count;
    unsigned bits;
};

FSTRACE_DECL(ASYNC_BASE64DECODER_CREATE, "UID=%64u PTR=%p ASYNC=%p STREAM=%p");

base64decoder_t *base64_decode(async_t *async, bytestream_1 stream, char pos62,
                               char pos63)
{
    base64decoder_t *decoder = fsalloc(sizeof(*decoder));
    decoder->async = async;
    decoder->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_BASE64DECODER_CREATE, decoder->uid, decoder, async,
            stream.obj);
    decoder->stream = stream;
    decoder->pos62 = pos62 == -1 ? '+' : pos62;
    decoder->pos63 = pos63 == -1 ? '/' : pos63;
    decoder->bit_count = 0;
    decoder->bits = 0; /* don't-care */
    return decoder;
}

static int8_t map(base64decoder_t *decoder, uint8_t c)
{
    int8_t v = base64_bitfield_decoding[c];
    if (v != -1)
        return v;
    if (c == decoder->pos62)
        return 62;
    if (c == decoder->pos63)
        return 63;
    return -1;
}

FSTRACE_DECL(ASYNC_BASE64DECODER_READ_INPUT_DUMP, "UID=%64u TEXT=%A");

static ssize_t decoder_read(base64decoder_t *decoder, void *buf, size_t count)
{
    if (!count || decoder->bit_count == -1)
        return 0;
    uint8_t *q = buf;
    do {
        ssize_t n = bytestream_1_read(decoder->stream, buf, count);
        if (n < 0)
            return -1;
        if (n == 0) {
            decoder->bit_count = -1;
            break;
        }
        FSTRACE(ASYNC_BASE64DECODER_READ_INPUT_DUMP, decoder->uid, buf, n);
        uint8_t *p = buf;
        while (n--) {
            int v = map(decoder, *p++);
            if (v < 0)
                continue;
            decoder->bits = decoder->bits << 6 | v;
            decoder->bit_count += 6;
            if (decoder->bit_count >= 8) {
                decoder->bit_count -= 8;
                *q++ = decoder->bits >> decoder->bit_count;
            }
        }
    } while (q == buf);
    return q - (uint8_t *) buf;
}

FSTRACE_DECL(ASYNC_BASE64DECODER_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_BASE64DECODER_READ_DUMP, "UID=%64u DATA=%B");

ssize_t base64decoder_read(base64decoder_t *decoder, void *buf, size_t count)
{
    ssize_t n = decoder_read(decoder, buf, count);
    FSTRACE(ASYNC_BASE64DECODER_READ, decoder->uid, count, n);
    FSTRACE(ASYNC_BASE64DECODER_READ_DUMP, decoder->uid, buf, n);
    return n;
}

FSTRACE_DECL(ASYNC_BASE64DECODER_CLOSE, "UID=%64u");

void base64decoder_close(base64decoder_t *decoder)
{
    assert(decoder->async);
    FSTRACE(ASYNC_BASE64DECODER_CLOSE, decoder->uid);
    bytestream_1_close(decoder->stream);
    async_wound(decoder->async, decoder);
    decoder->async = NULL;
}

FSTRACE_DECL(ASYNC_BASE64DECODER_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void base64decoder_register_callback(base64decoder_t *decoder, action_1 action)
{
    FSTRACE(ASYNC_BASE64DECODER_REGISTER, decoder->uid, action.obj, action.act);
    bytestream_1_register_callback(decoder->stream, action);
}

FSTRACE_DECL(ASYNC_BASE64DECODER_UNREGISTER, "UID=%64u");

void base64decoder_unregister_callback(base64decoder_t *decoder)
{
    FSTRACE(ASYNC_BASE64DECODER_UNREGISTER, decoder->uid);
    bytestream_1_unregister_callback(decoder->stream);
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return base64decoder_read(obj, buf, count);
}

static void _close(void *obj)
{
    base64decoder_close(obj);
}

static void _register_callback(void *obj, action_1 action)
{
    return base64decoder_register_callback(obj, action);
}

static void _unregister_callback(void *obj)
{
    return base64decoder_unregister_callback(obj);
}

static struct bytestream_1_vt base64stream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

bytestream_1 base64decoder_as_bytestream_1(base64decoder_t *decoder)
{
    return (bytestream_1) { decoder, &base64stream_vt };
}
