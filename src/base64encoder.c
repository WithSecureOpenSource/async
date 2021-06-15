#include "base64encoder.h"

#include <assert.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"

enum {
    BASE64ENCODER_PAD = -1,
    BASE64ENCODER_PAD2 = -2,
    BASE64ENCODER_EOF = -3
};

struct base64encoder {
    async_t *async;
    uint64_t uid;
    bytestream_1 stream;
    char pos62, pos63, padchar;
    bool pad;
    size_t bit_count;
    unsigned bits;
};

static char BASE64MAP[62] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

FSTRACE_DECL(ASYNC_BASE64ENCODER_CREATE, "UID=%64u PTR=%p ASYNC=%p STREAM=%p");

base64encoder_t *base64_encode(async_t *async, bytestream_1 stream, char pos62,
                               char pos63, bool pad, char padchar)
{
    base64encoder_t *encoder = fsalloc(sizeof(*encoder));
    encoder->async = async;
    encoder->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_BASE64ENCODER_CREATE, encoder->uid, encoder, async,
            stream.obj);
    encoder->stream = stream;
    encoder->pos62 = pos62 == -1 ? '+' : pos62;
    encoder->pos63 = pos63 == -1 ? '/' : pos63;
    encoder->pad = pad;
    encoder->padchar = padchar == -1 ? '=' : padchar;
    encoder->bit_count = 0;
    encoder->bits = 0; /* don't-care */
    return encoder;
}

static char map(base64encoder_t *encoder, uint8_t n)
{
    switch (n) {
        case 62:
            return encoder->pos62;
        case 63:
            return encoder->pos63;
        default:
            return BASE64MAP[n];
    }
}

static ssize_t finalize(base64encoder_t *encoder, size_t count, char *q)
{
    switch (encoder->bit_count) {
        case 2:
            *q++ = map(encoder, encoder->bits << 4 & 0x3f);
            if (!encoder->pad) {
                encoder->bit_count = BASE64ENCODER_EOF;
                return 1;
            }
            if (count > 1) {
                *q++ = encoder->padchar;
                if (count > 2) {
                    *q = encoder->padchar;
                    encoder->bit_count = BASE64ENCODER_EOF;
                    return 3;
                }
                encoder->bit_count = BASE64ENCODER_PAD;
                return 2;
            }
            encoder->bit_count = BASE64ENCODER_PAD2;
            return 1;
        case 4:
            *q++ = map(encoder, encoder->bits << 2 & 0x3f);
            if (!encoder->pad) {
                encoder->bit_count = BASE64ENCODER_EOF;
                return 1;
            }
            if (count > 1) {
                *q = encoder->padchar;
                encoder->bit_count = BASE64ENCODER_EOF;
                return 2;
            }
            encoder->bit_count = BASE64ENCODER_PAD;
            return 1;
        default:
            encoder->bit_count = BASE64ENCODER_EOF;
            return 0;
    }
}

static ssize_t do_read(base64encoder_t *encoder, void *buf, size_t count)
{
    if (!count)
        return 0;
    char *q = buf;
    switch (encoder->bit_count) {
        case BASE64ENCODER_PAD2:
            *q++ = encoder->padchar;
            if (count > 1) {
                *q = encoder->padchar;
                encoder->bit_count = BASE64ENCODER_EOF;
                return 2;
            }
            encoder->bit_count = BASE64ENCODER_PAD;
            return 1;
        case BASE64ENCODER_PAD:
            *q = encoder->padchar;
            encoder->bit_count = BASE64ENCODER_EOF;
            return 1;
        case BASE64ENCODER_EOF:
            return 0;
        default:;
    }
    size_t need = (count * 6 + 7 - encoder->bit_count) / 8;
    assert(need > 0);
    uint8_t *p = (uint8_t *) buf + count - need;
    ssize_t n = bytestream_1_read(encoder->stream, p, need);
    if (n < 0)
        return -1;
    if (n == 0)
        return finalize(encoder, count, q);
    while (n--) {
        encoder->bits = encoder->bits << 8 | *p++;
        encoder->bit_count += 8;
        while (encoder->bit_count >= 6) {
            encoder->bit_count -= 6;
            *q++ = map(encoder, encoder->bits >> encoder->bit_count & 0x3f);
        }
    }
    assert(q - (char *) buf <= count);
    return q - (char *) buf;
}

FSTRACE_DECL(ASYNC_BASE64ENCODER_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_BASE64ENCODER_READ_DUMP, "UID=%64u TEXT=%A");

ssize_t base64encoder_read(base64encoder_t *encoder, void *buf, size_t count)
{
    ssize_t n = do_read(encoder, buf, count);
    FSTRACE(ASYNC_BASE64ENCODER_READ, encoder->uid, count, n);
    FSTRACE(ASYNC_BASE64ENCODER_READ_DUMP, encoder->uid, buf, n);
    return n;
}

FSTRACE_DECL(ASYNC_BASE64ENCODER_CLOSE, "UID=%64u");

void base64encoder_close(base64encoder_t *encoder)
{
    assert(encoder->async);
    FSTRACE(ASYNC_BASE64ENCODER_CLOSE, encoder->uid);
    bytestream_1_close(encoder->stream);
    async_wound(encoder->async, encoder);
    encoder->async = NULL;
}

FSTRACE_DECL(ASYNC_BASE64ENCODER_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void base64encoder_register_callback(base64encoder_t *encoder, action_1 action)
{
    FSTRACE(ASYNC_BASE64ENCODER_REGISTER, encoder->uid, action.obj, action.act);
    bytestream_1_register_callback(encoder->stream, action);
}

FSTRACE_DECL(ASYNC_BASE64ENCODER_UNREGISTER, "UID=%64u");

void base64encoder_unregister_callback(base64encoder_t *encoder)
{
    FSTRACE(ASYNC_BASE64ENCODER_UNREGISTER, encoder->uid);
    bytestream_1_unregister_callback(encoder->stream);
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return base64encoder_read(obj, buf, count);
}

static void _close(void *obj)
{
    base64encoder_close(obj);
}

static void _register_callback(void *obj, action_1 action)
{
    return base64encoder_register_callback(obj, action);
}

static void _unregister_callback(void *obj)
{
    return base64encoder_unregister_callback(obj);
}

static struct bytestream_1_vt base64stream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

bytestream_1 base64encoder_as_bytestream_1(base64encoder_t *encoder)
{
    return (bytestream_1) { encoder, &base64stream_vt };
}
