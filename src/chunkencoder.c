#include "chunkencoder.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"

struct chunkencoder {
    async_t *async;
    uint64_t uid;
    bytestream_1 stream;
    size_t max_chunk_size;
    uint8_t *chunkbuf, *eoc, *next;
    int chunk_count, eof_pending;
    chunkencoder_termination_t termination;
};

enum {
    MIN_CHUNK_SIZE = 2, /* the terminating CRLF */
    MAX_CHUNK_SIZE = 16 * 1024 * 1024,
    MAX_LENGTH_LENGTH = 2 + 7 + 2 /* CRLF plus 7 hex digits plus CRLF */
};

static const uint8_t hexdigit[] = "0123456789abcdef";

static ssize_t do_read(chunkencoder_t *encoder, void *buf, size_t count)
{
    if (count <= 0)
        return 0;
    if (encoder->next >= encoder->eoc) {
        if (encoder->eof_pending)
            return 0;
        ssize_t n = bytestream_1_read(encoder->stream,
                                      encoder->chunkbuf + MAX_LENGTH_LENGTH,
                                      encoder->max_chunk_size);
        if (n < 0)
            return n;
        if (n == 0) {
            encoder->eof_pending = 1;
            encoder->eoc = encoder->chunkbuf + MAX_LENGTH_LENGTH;
            switch (encoder->termination) {
                case CHUNKENCODER_SIMPLE:
                    *encoder->eoc++ = '\r';
                    *encoder->eoc++ = '\n';
                    break;
                case CHUNKENCODER_STOP_AT_TRAILER:
                    break;
                case CHUNKENCODER_STOP_AT_FINAL_EXTENSIONS:
                    encoder->eoc -= 2;
                    break;
                default:
                    abort();
            }
        } else
            encoder->eoc = encoder->chunkbuf + MAX_LENGTH_LENGTH + n;
        encoder->next = encoder->chunkbuf + MAX_LENGTH_LENGTH - 2;
        do {
            *--encoder->next = hexdigit[n % 16];
            n /= 16;
        } while (n != 0);
        if (encoder->chunk_count++ > 0) {
            *--encoder->next = '\n';
            *--encoder->next = '\r';
        }
    }
    ssize_t n = count;
    if (n > encoder->eoc - encoder->next)
        n = encoder->eoc - encoder->next;
    memcpy(buf, encoder->next, n);
    encoder->next += n;
    return n;
}

FSTRACE_DECL(ASYNC_CHUNKENCODER_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_CHUNKENCODER_READ_DUMP, "UID=%64u DATA=%B");

ssize_t chunkencoder_read(chunkencoder_t *encoder, void *buf, size_t count)
{
    ssize_t n = do_read(encoder, buf, count);
    FSTRACE(ASYNC_CHUNKENCODER_READ, encoder->uid, count, n);
    FSTRACE(ASYNC_CHUNKENCODER_READ_DUMP, encoder->uid, buf, n);
    return n;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return chunkencoder_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_CHUNKENCODER_CLOSE, "UID=%64u");

void chunkencoder_close(chunkencoder_t *encoder)
{
    FSTRACE(ASYNC_CHUNKENCODER_CLOSE, encoder->uid);
    assert(encoder->async != NULL);
    bytestream_1_close(encoder->stream);
    fsfree(encoder->chunkbuf);
    async_wound(encoder->async, encoder);
    encoder->async = NULL;
}

static void _close(void *obj)
{
    chunkencoder_close(obj);
}

FSTRACE_DECL(ASYNC_CHUNKENCODER_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void chunkencoder_register_callback(chunkencoder_t *encoder, action_1 action)
{
    FSTRACE(ASYNC_CHUNKENCODER_REGISTER, encoder->uid, action.obj, action.act);
    bytestream_1_register_callback(encoder->stream, action);
}

static void _register_callback(void *obj, action_1 action)
{
    chunkencoder_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_CHUNKENCODER_UNREGISTER, "UID=%64u");

void chunkencoder_unregister_callback(chunkencoder_t *encoder)
{
    FSTRACE(ASYNC_CHUNKENCODER_UNREGISTER, encoder->uid);
    bytestream_1_unregister_callback(encoder->stream);
}

static void _unregister_callback(void *obj)
{
    chunkencoder_unregister_callback(obj);
}

static const struct bytestream_1_vt chunkencoder_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 chunkencoder_as_bytestream_1(chunkencoder_t *encoder)
{
    return (bytestream_1) { encoder, &chunkencoder_vt };
}

static const char *trace_termination(void *ptermination)
{
    switch (*(chunkencoder_termination_t *) ptermination) {
        case CHUNKENCODER_SIMPLE:
            return "CHUNKENCODER_SIMPLE";
        case CHUNKENCODER_STOP_AT_TRAILER:
            return "CHUNKENCODER_STOP_AT_TRAILER";
        case CHUNKENCODER_STOP_AT_FINAL_EXTENSIONS:
            return "CHUNKENCODER_STOP_AT_FINAL_EXTENSIONS";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNC_CHUNKENCODER_CREATE,
             "UID=%64u PTR=%p ASYNC=%p STREAM=%p MAX-CHUNK=%z TERMINATION=%I");

chunkencoder_t *chunk_encode_2(async_t *async, bytestream_1 stream,
                               size_t max_chunk_size,
                               chunkencoder_termination_t termination)
{
    chunkencoder_t *encoder = fsalloc(sizeof *encoder);
    encoder->async = async;
    encoder->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_CHUNKENCODER_CREATE, encoder->uid, encoder, async, stream.obj,
            max_chunk_size, trace_termination, &termination);
    encoder->stream = stream;
    if (max_chunk_size > MAX_CHUNK_SIZE)
        encoder->max_chunk_size = MAX_CHUNK_SIZE;
    else if (max_chunk_size < MIN_CHUNK_SIZE)
        encoder->max_chunk_size = MIN_CHUNK_SIZE;
    else
        encoder->max_chunk_size = max_chunk_size;
    encoder->chunkbuf = fsalloc(encoder->max_chunk_size + MAX_LENGTH_LENGTH);
    encoder->chunkbuf[MAX_LENGTH_LENGTH - 2] = '\r';
    encoder->chunkbuf[MAX_LENGTH_LENGTH - 1] = '\n';
    encoder->eoc = encoder->next = encoder->chunkbuf;
    encoder->chunk_count = 0;
    encoder->eof_pending = 0;
    encoder->termination = termination;
    return encoder;
}

chunkencoder_t *chunk_encode(async_t *async, bytestream_1 stream,
                             size_t max_chunk_size)
{
    return chunk_encode_2(async, stream, max_chunk_size, CHUNKENCODER_SIMPLE);
}
