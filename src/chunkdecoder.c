#include "chunkdecoder.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <fsdyn/charstr.h>
#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"

typedef ssize_t (*chunkdecoder_state_t)(chunkdecoder_t *decoder, void *buf,
                                        size_t size);

struct chunkdecoder {
    async_t *async;
    uint64_t uid;
    bytestream_1 stream;
    int mode;
    chunkdecoder_state_t state, next_state;
    /* chunk_length is gradually composed in READING_LENGTH. Then, in
     * READING_CHUNK_DATA, it represents the remaining number of data
     * bytes to deliver. */
    size_t chunk_length;
    uint8_t buffer[32];
    size_t low, high;
};

static ssize_t replenish(chunkdecoder_t *decoder)
{
    ssize_t amount = bytestream_1_read(decoder->stream, decoder->buffer,
                                       sizeof decoder->buffer);
    if (amount >= 0) {
        decoder->low = 0;
        decoder->high = amount;
    }
    return amount;
}

static void transition(chunkdecoder_t *decoder, chunkdecoder_state_t state)
{
    decoder->next_state = state;
}

static ssize_t read_length(chunkdecoder_t *decoder, void *buf, size_t size);
static ssize_t read_extensions(chunkdecoder_t *decoder, void *buf, size_t size);
static ssize_t read_chunk_data(chunkdecoder_t *decoder, void *buf, size_t size);
static ssize_t read_chunk_terminator(chunkdecoder_t *decoder, void *buf,
                                     size_t size);
static ssize_t read_chunk_terminator_cr(chunkdecoder_t *decoder, void *buf,
                                        size_t size);
static ssize_t read_trailer(chunkdecoder_t *decoder, void *buf, size_t size);
static ssize_t read_trailer_skip(chunkdecoder_t *decoder, void *buf,
                                 size_t size);
static ssize_t read_trailer_cr(chunkdecoder_t *decoder, void *buf, size_t size);
static ssize_t read_exhausted_check_eof(chunkdecoder_t *decoder, void *buf,
                                        size_t size);
static ssize_t read_exhausted(chunkdecoder_t *decoder, void *buf, size_t size);
static ssize_t read_errored(chunkdecoder_t *decoder, void *buf, size_t size);

static ssize_t transition_error(chunkdecoder_t *decoder)
{
    transition(decoder, read_errored);
    return -1;
}

static ssize_t read_length(chunkdecoder_t *decoder, void *buf, size_t size)
{
    if (size == 0)
        return 0;
    for (;;) {
        for (; decoder->low < decoder->high; decoder->low++) {
            unsigned digit = charstr_digit_value(decoder->buffer[decoder->low]);
            if (digit == -1U) {
                if (decoder->chunk_length == 0 &&
                    decoder->mode == CHUNKDECODER_DETACH_AT_FINAL_EXTENSIONS)
                    transition(decoder, read_exhausted);
                else
                    transition(decoder, read_extensions);
                return -1;
            }
            if (decoder->chunk_length > SIZE_MAX / 16)
                return transition_error(decoder); /* overflow */
            decoder->chunk_length *= 16;
            if (digit > SIZE_MAX - decoder->chunk_length)
                return transition_error(decoder); /* overflow */
            decoder->chunk_length += digit;
        }
        ssize_t amount = replenish(decoder);
        if (amount < 0)
            return -1;
        if (amount == 0)
            return transition_error(decoder);
    }
}

static ssize_t read_extensions(chunkdecoder_t *decoder, void *buf, size_t size)
{
    /* Not implemented; simply skip */
    if (size == 0)
        return 0;
    for (;;) {
        while (decoder->low < decoder->high)
            if (decoder->buffer[decoder->low++] == '\n') {
                if (decoder->chunk_length > 0)
                    transition(decoder, read_chunk_data);
                else if (decoder->mode == CHUNKDECODER_DETACH_AT_TRAILER)
                    transition(decoder, read_exhausted);
                else
                    transition(decoder, read_trailer);
                return -1;
            }
        ssize_t amount = replenish(decoder);
        if (amount < 0)
            return -1;
        if (amount == 0)
            return transition_error(decoder);
    }
}

static ssize_t read_chunk_data(chunkdecoder_t *decoder, void *buf, size_t size)
{
    if (size == 0)
        return 0;
    if (decoder->chunk_length == 0) {
        transition(decoder, read_chunk_terminator);
        return -1;
    }
    size_t available = decoder->high - decoder->low;
    if (available == 0) {
        if (size > decoder->chunk_length)
            size = decoder->chunk_length;
        ssize_t amount = bytestream_1_read(decoder->stream, buf, size);
        if (amount < 0)
            return -1;
        if (amount == 0)
            return transition_error(decoder);
        decoder->chunk_length -= amount;
        return amount;
    }
    if (available > decoder->chunk_length)
        available = decoder->chunk_length;
    if (available < size)
        size = available;
    memcpy(buf, decoder->buffer + decoder->low, size);
    decoder->low += size;
    decoder->chunk_length -= size;
    return size;
}

static ssize_t read_chunk_terminator(chunkdecoder_t *decoder, void *buf,
                                     size_t size)
{
    if (size == 0)
        return 0;
    if (decoder->low == decoder->high) {
        ssize_t amount = replenish(decoder);
        if (amount < 0)
            return -1;
        if (amount == 0)
            return transition_error(decoder);
    }
    switch (decoder->buffer[decoder->low++]) {
        case '\n':
            assert(decoder->chunk_length == 0);
            transition(decoder, read_length);
            break;
        case '\r':
            transition(decoder, read_chunk_terminator_cr);
            break;
        default:
            return transition_error(decoder);
    }
    return -1;
}

static ssize_t read_chunk_terminator_cr(chunkdecoder_t *decoder, void *buf,
                                        size_t size)
{
    if (size == 0)
        return 0;
    if (decoder->low == decoder->high) {
        ssize_t amount = replenish(decoder);
        if (amount < 0)
            return -1;
        if (amount == 0)
            return transition_error(decoder);
    }
    switch (decoder->buffer[decoder->low++]) {
        case '\n':
            assert(decoder->chunk_length == 0);
            transition(decoder, read_length);
            break;
        default:
            return transition_error(decoder);
    }
    return -1;
}

static ssize_t read_trailer(chunkdecoder_t *decoder, void *buf, size_t size)
{
    if (size == 0)
        return 0;
    if (decoder->low == decoder->high) {
        ssize_t amount = replenish(decoder);
        if (amount < 0)
            return -1;
        if (amount == 0)
            return transition_error(decoder);
    }
    switch (decoder->buffer[decoder->low++]) {
        case '\n':
            if (decoder->mode == CHUNKDECODER_ADOPT_INPUT)
                transition(decoder, read_exhausted_check_eof);
            else
                transition(decoder, read_exhausted);
            break;
        case '\r':
            transition(decoder, read_trailer_cr);
            break;
        default:
            transition(decoder, read_trailer_skip);
            break;
    }
    return -1;
}

static ssize_t read_trailer_skip(chunkdecoder_t *decoder, void *buf,
                                 size_t size)
{
    if (size == 0)
        return 0;
    for (;;) {
        while (decoder->low < decoder->high)
            if (decoder->buffer[decoder->low++] == '\n') {
                transition(decoder, read_trailer);
                return -1;
            }
        ssize_t amount = replenish(decoder);
        if (amount < 0)
            return -1;
        if (amount == 0)
            return transition_error(decoder);
    }
}

static ssize_t read_trailer_cr(chunkdecoder_t *decoder, void *buf, size_t size)
{
    if (size == 0)
        return 0;
    if (decoder->low == decoder->high) {
        ssize_t amount = replenish(decoder);
        if (amount < 0)
            return -1;
        if (amount == 0)
            return transition_error(decoder);
    }
    switch (decoder->buffer[decoder->low++]) {
        case '\n':
            if (decoder->mode == CHUNKDECODER_ADOPT_INPUT)
                transition(decoder, read_exhausted_check_eof);
            else
                transition(decoder, read_exhausted);
            break;
        default:
            transition(decoder, read_trailer_skip);
            break;
    }
    return -1;
}

static ssize_t read_exhausted_check_eof(chunkdecoder_t *decoder, void *buf,
                                        size_t size)
{
    if (chunkdecoder_leftover_size(decoder) > 0)
        return transition_error(decoder);
    char c;
    ssize_t count = bytestream_1_read(decoder->stream, &c, 1);
    if (count < 0)
        return -1;
    if (count > 0)
        return transition_error(decoder);
    transition(decoder, read_exhausted);
    return -1;
}

static ssize_t read_exhausted(chunkdecoder_t *decoder, void *buf, size_t size)
{
    return 0;
}

static ssize_t read_errored(chunkdecoder_t *decoder, void *buf, size_t size)
{
    errno = EPROTO;
    return -1;
}

FSTRACE_DECL(ASYNC_CHUNKDECODER_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_CHUNKDECODER_READ_DUMP, "UID=%64u DATA=%B");

ssize_t chunkdecoder_read(chunkdecoder_t *decoder, void *buf, size_t size)
{
    ssize_t count;
    for (;;) {
        decoder->next_state = NULL;
        count = decoder->state(decoder, buf, size);
        if (!decoder->next_state) {
            FSTRACE(ASYNC_CHUNKDECODER_READ, decoder->uid, size, count);
            FSTRACE(ASYNC_CHUNKDECODER_READ_DUMP, decoder->uid, buf, count);
            return count;
        }
        decoder->state = decoder->next_state;
    }
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return chunkdecoder_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_CHUNKDECODER_CLOSE, "UID=%64u");

void chunkdecoder_close(chunkdecoder_t *decoder)
{
    FSTRACE(ASYNC_CHUNKDECODER_CLOSE, decoder->uid);
    assert(decoder->async != NULL);
    if (decoder->mode == CHUNKDECODER_ADOPT_INPUT)
        bytestream_1_close(decoder->stream);
    async_wound(decoder->async, decoder);
    decoder->async = NULL;
}

static void _close(void *obj)
{
    chunkdecoder_close(obj);
}

FSTRACE_DECL(ASYNC_CHUNKDECODER_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void chunkdecoder_register_callback(chunkdecoder_t *decoder, action_1 action)
{
    FSTRACE(ASYNC_CHUNKDECODER_REGISTER, decoder->uid, action.obj, action.act);
    bytestream_1_register_callback(decoder->stream, action);
}

static void _register_callback(void *obj, action_1 action)
{
    chunkdecoder_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_CHUNKDECODER_UNREGISTER, "UID=%64u");

void chunkdecoder_unregister_callback(chunkdecoder_t *decoder)
{
    FSTRACE(ASYNC_CHUNKDECODER_UNREGISTER, decoder->uid);
    bytestream_1_unregister_callback(decoder->stream);
}

static void _unregister_callback(void *obj)
{
    chunkdecoder_unregister_callback(obj);
}

static ssize_t _remaining(void *obj)
{
    errno = ENOTSUP;
    return -1;
}

size_t chunkdecoder_leftover_size(chunkdecoder_t *decoder)
{
    return decoder->high - decoder->low;
}

static ssize_t _leftover_size(void *obj)
{
    return chunkdecoder_leftover_size(obj);
}

void *chunkdecoder_leftover_bytes(chunkdecoder_t *decoder)
{
    return decoder->buffer + decoder->low;
}

static void *_leftover_bytes(void *obj)
{
    return chunkdecoder_leftover_bytes(obj);
}

static const struct bytestream_2_vt chunkdecoder_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
    .remaining = _remaining,
    .leftover_size = _leftover_size,
    .leftover_bytes = _leftover_bytes,
};

bytestream_2 chunkdecoder_as_bytestream_2(chunkdecoder_t *decoder)
{
    return (bytestream_2) { decoder, &chunkdecoder_vt };
}

bytestream_1 chunkdecoder_as_bytestream_1(chunkdecoder_t *decoder)
{
    bytestream_2 stream = chunkdecoder_as_bytestream_2(decoder);
    return bytestream_2_as_bytestream_1(stream);
}

static const char *trace_mode(void *pmode)
{
    switch (*(int *) pmode) {
        case CHUNKDECODER_DETACH_AT_TRAILER:
            return "CHUNKDECODER_DETACH_AT_TRAILER";
        case CHUNKDECODER_DETACH_AFTER_TRAILER:
            return "CHUNKDECODER_DETACH_AFTER_TRAILER";
        case CHUNKDECODER_ADOPT_INPUT:
            return "CHUNKDECODER_ADOPT_INPUT";
        case CHUNKDECODER_DETACH_AT_FINAL_EXTENSIONS:
            return "CHUNKDECODER_DETACH_AT_FINAL_EXTENSIONS";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNC_CHUNKDECODER_CREATE,
             "UID=%64u PTR=%p ASYNC=%p STREAM=%p MODE=%I");

chunkdecoder_t *chunk_decode(async_t *async, bytestream_1 stream, int mode)
{
    chunkdecoder_t *decoder = fsalloc(sizeof *decoder);
    decoder->async = async;
    decoder->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_CHUNKDECODER_CREATE, decoder->uid, decoder, async, stream.obj,
            trace_mode, &mode);
    decoder->stream = stream;
    decoder->mode = mode;
    decoder->state = read_length;
    decoder->chunk_length = 0;
    decoder->low = 0;
    decoder->high = 0;
    return decoder;
}
