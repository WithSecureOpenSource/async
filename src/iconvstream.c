#include "iconvstream.h"

#include <assert.h>
#include <errno.h>
#include <iconv.h>
#include <string.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"

typedef enum {
    ICONVSTREAM_OPEN,
    ICONVSTREAM_EXHAUSTED,
    ICONVSTREAM_ERRORED,
    ICONVSTREAM_CLOSED
} iconvstream_state_t;

struct iconvstream {
    iconvstream_state_t state;
    int err;
    async_t *async;
    uint64_t uid;
    bytestream_1 source;
    iconv_t iconv;
    char inbuf[0x1000], *readp_in, *end_in;
    char outbuf[0x1000], *readp_out, *end_out;
};

FSTRACE_DECL(ASYNC_ICONVSTREAM_CREATE,
             "UID=%64u PTR=%p ASYNC=%p SOURCE=%p TO=%s FROM=%s");

iconvstream_t *open_iconvstream(async_t *async, bytestream_1 source,
                                const char *tocode, const char *fromcode)
{
    iconv_t iconv = iconv_open(tocode, fromcode);
    if (iconv == (iconv_t) -1)
        return NULL;
    iconvstream_t *stream = fsalloc(sizeof(*stream));
    stream->async = async;
    stream->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_ICONVSTREAM_CREATE, stream->uid, stream, async, source.obj,
            tocode, fromcode);
    stream->source = source;
    stream->iconv = iconv;
    stream->state = ICONVSTREAM_OPEN;
    stream->readp_in = stream->end_in = stream->inbuf;
    stream->readp_out = stream->end_out = stream->outbuf;
    return stream;
}

static const char *trace_state(void *pstate)
{
    switch (*(iconvstream_state_t *) pstate) {
        case ICONVSTREAM_OPEN:
            return "ICONVSTREAM_OPEN";
        case ICONVSTREAM_EXHAUSTED:
            return "ICONVSTREAM_EXHAUSTED";
        case ICONVSTREAM_ERRORED:
            return "ICONVSTREAM_ERRORED";
        case ICONVSTREAM_CLOSED:
            return "ICONVSTREAM_CLOSED";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNC_ICONVSTREAMNAIVEFRAMER_SET_STATE, "UID=%64u OLD=%I NEW=%I");

static void set_state(iconvstream_t *stream, iconvstream_state_t state)
{
    FSTRACE(ASYNC_ICONVSTREAMNAIVEFRAMER_SET_STATE, stream->uid, trace_state,
            &stream->state, trace_state, &state);
    stream->state = state;
}

FSTRACE_DECL(ASYNC_ICONVSTREAM_READ_INPUT_DUMP, "UID=%64u TEXT=%A");

static ssize_t consume_outbuf(iconvstream_t *stream, void *buf, size_t count)
{
    size_t left = stream->end_out - stream->readp_out;
    if (left < count)
        count = left;
    memcpy(buf, stream->readp_out, count);
    stream->readp_out += count;
    return count;
}

bool convert_inbuf(iconvstream_t *stream)
{
    size_t left_in = stream->end_in - stream->readp_in;
    size_t left_out = sizeof stream->outbuf;
    stream->readp_out = stream->end_out = stream->outbuf;
    ssize_t k = iconv(stream->iconv, &stream->readp_in, &left_in,
                      &stream->end_out, &left_out);
    if (stream->end_out != stream->outbuf)
        return true;
    if (k >= 0) {
        assert(left_in == 0);
        errno = EAGAIN;
        return false;
    }
    switch (errno) {
        case EINVAL:
            errno = EAGAIN;
            return false;
        default:
            set_state(stream, ICONVSTREAM_ERRORED);
            stream->err = errno;
            return false;
    }
}

static ssize_t stream_read(iconvstream_t *stream, void *buf, size_t count)
{
    if (count <= 0)
        return 0;
    switch (stream->state) {
        case ICONVSTREAM_OPEN:
            break;
        case ICONVSTREAM_EXHAUSTED:
            return 0;
        case ICONVSTREAM_ERRORED:
            errno = stream->err;
            return -1;
        default:
            assert(false);
    }
    for (;;) {
        if (stream->readp_out < stream->end_out || convert_inbuf(stream))
            return consume_outbuf(stream, buf, count);
        if (errno != EAGAIN)
            return -1;
        size_t left_in = stream->end_in - stream->readp_in;
        memmove(stream->inbuf, stream->readp_in, left_in);
        stream->readp_in = stream->inbuf;
        stream->end_in = stream->inbuf + left_in;
        ssize_t n = bytestream_1_read(stream->source, stream->end_in,
                                      sizeof stream->inbuf - left_in);
        if (n < 0)
            return -1;
        if (!n) {
            if (left_in) {
                set_state(stream, ICONVSTREAM_ERRORED);
                stream->err = errno = EILSEQ;
                return -1;
            }
            set_state(stream, ICONVSTREAM_EXHAUSTED);
            return 0;
        }
        FSTRACE(ASYNC_ICONVSTREAM_READ_INPUT_DUMP, stream->uid, stream->inbuf,
                n);
        stream->end_in += n;
    }
}

FSTRACE_DECL(ASYNC_ICONVSTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_ICONVSTREAM_READ_DUMP, "UID=%64u TEXT=%A");

ssize_t iconvstream_read(iconvstream_t *stream, void *buf, size_t count)
{
    ssize_t n = stream_read(stream, buf, count);
    FSTRACE(ASYNC_ICONVSTREAM_READ, stream->uid, count, n);
    FSTRACE(ASYNC_ICONVSTREAM_READ_DUMP, stream->uid, buf, n);
    return n;
}

FSTRACE_DECL(ASYNC_ICONVSTREAM_CLOSE, "UID=%64u");

void iconvstream_close(iconvstream_t *stream)
{
    assert(stream->state != ICONVSTREAM_CLOSED);
    FSTRACE(ASYNC_ICONVSTREAM_CLOSE, stream->uid);
    bytestream_1_close(stream->source);
    iconv_close(stream->iconv);
    async_wound(stream->async, stream);
}

FSTRACE_DECL(ASYNC_ICONVSTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void iconvstream_register_callback(iconvstream_t *stream, action_1 action)
{
    FSTRACE(ASYNC_ICONVSTREAM_REGISTER, stream->uid, action.obj, action.act);
    bytestream_1_register_callback(stream->source, action);
}

FSTRACE_DECL(ASYNC_ICONVSTREAM_UNREGISTER, "UID=%64u");

void iconvstream_unregister_callback(iconvstream_t *stream)
{
    FSTRACE(ASYNC_ICONVSTREAM_UNREGISTER, stream->uid);
    bytestream_1_unregister_callback(stream->source);
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return iconvstream_read(obj, buf, count);
}

static void _close(void *obj)
{
    iconvstream_close(obj);
}

static void _register_callback(void *obj, action_1 action)
{
    return iconvstream_register_callback(obj, action);
}

static void _unregister_callback(void *obj)
{
    return iconvstream_unregister_callback(obj);
}

static struct bytestream_1_vt iconvstream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

bytestream_1 iconvstream_as_bytestream_1(iconvstream_t *stream)
{
    return (bytestream_1) { stream, &iconvstream_vt };
}
