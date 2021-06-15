#include "multipartdecoder.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <fsdyn/bytearray.h>
#include <fsdyn/charstr.h>
#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"

typedef enum {
    MULTIPARTDECODER_READING_DASH_BOUNDARY,
    MULTIPARTDECODER_AFTER_DASH_BOUNDARY,
    MULTIPARTDECODER_AFTER_FIRST_CR,
    MULTIPARTDECODER_READING_PART,
    MULTIPARTDECODER_AFTER_DELIMITER,
    MULTIPARTDECODER_READING_PADDING,
    MULTIPARTDECODER_AFTER_CR,
    MULTIPARTDECODER_AFTER_HYPHEN,
    MULTIPARTDECODER_SKIPPING,
    MULTIPARTDECODER_EOF,
    MULTIPARTDECODER_ERRORED,
    MULTIPARTDECODER_CLOSED
} multipartdecoder_state_t;

struct multipartdecoder {
    async_t *async;
    uint64_t uid;
    int pending_errno;
    bytestream_1 source;
    action_1 callback;
    multipartdecoder_state_t state;
    char *delimiter;
    size_t delimiter_cursor;
    byte_array_t *output_buffer;
    size_t output_cursor;
    char buffer[1024];
    size_t low, high;
};

FSTRACE_DECL(ASYNC_MULTIPARTDECODER_CREATE,
             "UID=%64u PTR=%p ASYNC=%p SOURCE=%p BOUNDARY=%s");

multipartdecoder_t *multipart_decode(async_t *async, bytestream_1 source,
                                     const char *boundary, bool first_part)
{
    multipartdecoder_t *decoder = fsalloc(sizeof *decoder);
    decoder->async = async;
    decoder->uid = fstrace_get_unique_id();
    decoder->pending_errno = 0;
    FSTRACE(ASYNC_MULTIPARTDECODER_CREATE, decoder->uid, decoder, async,
            source.obj, boundary);
    decoder->source = source;
    decoder->callback = NULL_ACTION_1;
    decoder->delimiter = charstr_printf("\r\n--%s", boundary);
    if (first_part) {
        decoder->state = MULTIPARTDECODER_READING_DASH_BOUNDARY;
        /* we match the dash-boundary using the delimiter starting at
         * offset 2, since delimiter := CRLF dash-boundary
         */
        decoder->delimiter_cursor = 2;
    } else {
        decoder->state = MULTIPARTDECODER_READING_PART;
        decoder->delimiter_cursor = 0;
    }
    decoder->output_buffer = make_byte_array(strlen(decoder->delimiter));
    decoder->output_cursor = 0;
    decoder->low = decoder->high = 0;
    return decoder;
}

static const char *trace_state(void *pstate)
{
    switch (*(multipartdecoder_state_t *) pstate) {
        case MULTIPARTDECODER_READING_DASH_BOUNDARY:
            return "MULTIPARTDECODER_READING_DASH_BOUNDARY";
        case MULTIPARTDECODER_AFTER_DASH_BOUNDARY:
            return "MULTIPARTDECODER_AFTER_DASH_BOUNDARY";
        case MULTIPARTDECODER_AFTER_FIRST_CR:
            return "MULTIPARTDECODER_AFTER_FIRST_CR";
        case MULTIPARTDECODER_READING_PART:
            return "MULTIPARTDECODER_READING_PART";
        case MULTIPARTDECODER_AFTER_DELIMITER:
            return "MULTIPARTDECODER_AFTER_DELIMITER";
        case MULTIPARTDECODER_READING_PADDING:
            return "MULTIPARTDECODER_READING_PADDING";
        case MULTIPARTDECODER_AFTER_CR:
            return "MULTIPARTDECODER_AFTER_CR";
        case MULTIPARTDECODER_AFTER_HYPHEN:
            return "MULTIPARTDECODER_AFTER_HYPHEN";
        case MULTIPARTDECODER_SKIPPING:
            return "MULTIPARTDECODER_SKIPPING";
        case MULTIPARTDECODER_EOF:
            return "MULTIPARTDECODER_EOF";
        case MULTIPARTDECODER_ERRORED:
            return "MULTIPARTDECODER_ERRORED";
        case MULTIPARTDECODER_CLOSED:
            return "MULTIPARTDECODER_CLOSED";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNC_MULTIPARTDECODER_SET_STATE, "UID=%64u OLD=%I NEW=%I");

static void set_decoder_state(multipartdecoder_t *decoder,
                              multipartdecoder_state_t state)
{
    FSTRACE(ASYNC_MULTIPARTDECODER_SET_STATE, decoder->uid, trace_state,
            &decoder->state, trace_state, &state);
    decoder->state = state;
}

static ssize_t skip_data(multipartdecoder_t *decoder)
{
    ssize_t count = bytestream_1_read(decoder->source, decoder->buffer,
                                      sizeof decoder->buffer);
    if (count < 0)
        return -1;
    if (!count) {
        set_decoder_state(decoder, MULTIPARTDECODER_EOF);
        return 0;
    }
    async_execute(decoder->async, decoder->callback);
    errno = EAGAIN;
    return -1;
}

/*
 *  RFC 2046 grammar
 *
 *  bcharsnospace := DIGIT / ALPHA / "'" / "(" / ")" /
 *                   "+" / "_" / "," / "-" / "." /
 *                   "/" / ":" / "=" / "?"
 *
 *  bchars := bcharsnospace / " "
 *
 *  boundary := 0*69<bchars> bcharsnospace
 *
 *  dash-boundary := "--" boundary
 *
 *  delimiter := CRLF dash-boundary
 *
 *  transport-padding := *LWSP-char
 *
 *  encapsulation := delimiter transport-padding
 *                   CRLF body-part
 *
 *  close-delimiter := delimiter "--"
 *
 *  discard-text := *(*text CRLF) *text
 *
 *  preamble := discard-text
 *
 *  epilogue := discard-text
 *
 *  multipart-body := [preamble CRLF]
 *                    dash-boundary transport-padding CRLF
 *                    body-part *encapsulation
 *                    close-delimiter transport-padding
 *                    [CRLF epilogue]
 */

static void read_symbol(multipartdecoder_t *decoder, char c)
{
    switch (decoder->state) {
        case MULTIPARTDECODER_READING_DASH_BOUNDARY:
            if (c == decoder->delimiter[decoder->delimiter_cursor]) {
                decoder->delimiter_cursor++;
                if (!decoder->delimiter[decoder->delimiter_cursor]) {
                    decoder->delimiter_cursor = 0;
                    set_decoder_state(decoder,
                                      MULTIPARTDECODER_AFTER_DASH_BOUNDARY);
                }
            } else
                decoder->delimiter_cursor = 2;
            break;
        case MULTIPARTDECODER_AFTER_DASH_BOUNDARY:
            switch (c) {
                case ' ':
                case '\t':
                    break;
                case '\r':
                    set_decoder_state(decoder, MULTIPARTDECODER_AFTER_FIRST_CR);
                    break;
                default:
                    set_decoder_state(decoder, MULTIPARTDECODER_ERRORED);
                    break;
            }
            break;
        case MULTIPARTDECODER_AFTER_FIRST_CR:
            switch (c) {
                case '\n':
                    set_decoder_state(decoder, MULTIPARTDECODER_READING_PART);
                    break;
                default:
                    set_decoder_state(decoder, MULTIPARTDECODER_ERRORED);
                    break;
            }
            break;
        case MULTIPARTDECODER_READING_PART:
            if (c == decoder->delimiter[decoder->delimiter_cursor]) {
                decoder->delimiter_cursor++;
                if (!decoder->delimiter[decoder->delimiter_cursor]) {
                    decoder->delimiter_cursor = 0;
                    set_decoder_state(decoder,
                                      MULTIPARTDECODER_AFTER_DELIMITER);
                }
            } else {
                /* We rely on the fact that no delimiter prefix is
                 * also a suffix of a (longer) delimiter prefix
                 * (because CR and LF are not legal in the
                 * boundary), i.e., we can restart matching the
                 * delimiter from the first position.
                 */
                decoder->output_cursor = 0;
                byte_array_clear(decoder->output_buffer);
                byte_array_append(decoder->output_buffer, decoder->delimiter,
                                  decoder->delimiter_cursor);
                decoder->delimiter_cursor = 0;
                if (c == decoder->delimiter[0])
                    decoder->delimiter_cursor++;
                else
                    byte_array_append(decoder->output_buffer, &c, 1);
            }
            break;
        case MULTIPARTDECODER_AFTER_DELIMITER:
            switch (c) {
                case ' ':
                case '\t':
                    set_decoder_state(decoder,
                                      MULTIPARTDECODER_READING_PADDING);
                    break;
                case '\r':
                    set_decoder_state(decoder, MULTIPARTDECODER_AFTER_CR);
                    break;
                case '-':
                    set_decoder_state(decoder, MULTIPARTDECODER_AFTER_HYPHEN);
                    break;
                default:
                    set_decoder_state(decoder, MULTIPARTDECODER_ERRORED);
                    break;
            }
            break;
        case MULTIPARTDECODER_READING_PADDING:
            switch (c) {
                case ' ':
                case '\t':
                    break;
                case '\r':
                    set_decoder_state(decoder, MULTIPARTDECODER_AFTER_CR);
                    break;
                default:
                    set_decoder_state(decoder, MULTIPARTDECODER_ERRORED);
                    break;
            }
            break;
        case MULTIPARTDECODER_AFTER_CR:
            switch (c) {
                case '\n':
                    set_decoder_state(decoder, MULTIPARTDECODER_EOF);
                    break;
                default:
                    set_decoder_state(decoder, MULTIPARTDECODER_ERRORED);
                    break;
            }
            break;
        case MULTIPARTDECODER_AFTER_HYPHEN:
            switch (c) {
                case '-':
                    set_decoder_state(decoder, MULTIPARTDECODER_SKIPPING);
                    break;
                default:
                    set_decoder_state(decoder, MULTIPARTDECODER_ERRORED);
                    break;
            }
            break;
        default:
            assert(false);
    }
}

FSTRACE_DECL(ASYNC_MULTIPARTDECODER_INPUT_DUMP, "UID=%64u TEXT=%A");

static ssize_t do_read(multipartdecoder_t *decoder, void *buf, size_t size)
{
    switch (decoder->state) {
        case MULTIPARTDECODER_SKIPPING:
            return skip_data(decoder);
        case MULTIPARTDECODER_EOF:
            errno = 0;
            return 0;
        case MULTIPARTDECODER_ERRORED:
            errno = EPROTO;
            return -1;
        case MULTIPARTDECODER_CLOSED:
            errno = EBADF;
            return -1;
        default:
            break;
    }

    if (decoder->pending_errno) {
        errno = decoder->pending_errno;
        decoder->pending_errno = 0;
        return -1;
    }

    size_t cursor = 0;
    char *ptr = buf;
    while (cursor < size) {
        if (decoder->low >= decoder->high) {
            ssize_t count = bytestream_1_read(decoder->source, decoder->buffer,
                                              sizeof decoder->buffer);
            if (count < 0) {
                if (!cursor)
                    return -1;
                if (errno != EAGAIN)
                    decoder->pending_errno = errno;
                break;
            }
            if (!count) {
                errno = EPROTO;
                return -1;
            }
            FSTRACE(ASYNC_MULTIPARTDECODER_INPUT_DUMP, decoder->uid,
                    decoder->buffer, count);
            decoder->low = 0;
            decoder->high = count;
        }
        if (decoder->output_cursor < byte_array_size(decoder->output_buffer)) {
            const char *data = byte_array_data(decoder->output_buffer);
            ptr[cursor++] = data[decoder->output_cursor++];
        } else {
            char c = decoder->buffer[decoder->low++];
            read_symbol(decoder, c);
            switch (decoder->state) {
                case MULTIPARTDECODER_SKIPPING:
                    decoder->low = decoder->high = 0;
                    if (cursor)
                        return cursor;
                    return skip_data(decoder);
                case MULTIPARTDECODER_EOF:
                    return cursor;
                case MULTIPARTDECODER_ERRORED:
                    errno = EPROTO;
                    return -1;
                default:
                    break;
            }
        }
    }

    return cursor;
}

FSTRACE_DECL(ASYNC_MULTIPARTDECODER_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_MULTIPARTDECODER_READ_DUMP, "UID=%64u DATA=%B");

ssize_t multipartdecoder_read(multipartdecoder_t *decoder, void *buf,
                              size_t size)
{
    ssize_t count = do_read(decoder, buf, size);
    FSTRACE(ASYNC_MULTIPARTDECODER_READ, decoder->uid, size, count);
    FSTRACE(ASYNC_MULTIPARTDECODER_READ_DUMP, decoder->uid, buf, count);
    return count;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return multipartdecoder_read(obj, buf, count);
}

void multipartdecoder_close(multipartdecoder_t *decoder)
{
    assert(decoder->state != MULTIPARTDECODER_CLOSED);
    decoder->callback = NULL_ACTION_1;
    destroy_byte_array(decoder->output_buffer);
    fsfree(decoder->delimiter);
    async_wound(decoder->async, decoder);
    set_decoder_state(decoder, MULTIPARTDECODER_CLOSED);
}

static void _close(void *obj)
{
    multipartdecoder_close(obj);
}

void multipartdecoder_register_callback(multipartdecoder_t *decoder,
                                        action_1 action)
{
    decoder->callback = action;
    bytestream_1_register_callback(decoder->source, action);
}

static void _register_callback(void *obj, action_1 action)
{
    multipartdecoder_register_callback(obj, action);
}

void multipartdecoder_unregister_callback(multipartdecoder_t *decoder)
{
    multipartdecoder_register_callback(decoder, NULL_ACTION_1);
}

static void _unregister_callback(void *obj)
{
    multipartdecoder_unregister_callback(obj);
}

static ssize_t _remaining(void *obj)
{
    errno = ENOTSUP;
    return -1;
}

size_t multipartdecoder_leftover_size(multipartdecoder_t *decoder)
{
    return decoder->high - decoder->low;
}

static ssize_t _leftover_size(void *obj)
{
    return multipartdecoder_leftover_size(obj);
}

void *multipartdecoder_leftover_bytes(multipartdecoder_t *decoder)
{
    return decoder->buffer + decoder->low;
}

static void *_leftover_bytes(void *obj)
{
    return multipartdecoder_leftover_bytes(obj);
}

static const struct bytestream_2_vt multipartdecoder_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
    .remaining = _remaining,
    .leftover_size = _leftover_size,
    .leftover_bytes = _leftover_bytes,
};

bytestream_2 multipartdecoder_as_bytestream_2(multipartdecoder_t *decoder)
{
    return (bytestream_2) { decoder, &multipartdecoder_vt };
}
