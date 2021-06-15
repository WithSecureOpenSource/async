#include "jsondecoder.h"

#include <assert.h>
#include <errno.h>

#include <fsdyn/bytearray.h>
#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"

struct jsondecoder {
    async_t *async;
    uint64_t uid;
    bytestream_1 source;
    byte_array_t *buffer;
    bool eof;
};

FSTRACE_DECL(ASYNC_JSONDECODER_CREATE,
             "UID=%64u PTR=%p ASYNC=%p SOURCE=%p MAX-SIZE=%z");

jsondecoder_t *open_jsondecoder(async_t *async, bytestream_1 source,
                                size_t max_encoding_size)
{
    jsondecoder_t *decoder = fsalloc(sizeof *decoder);
    decoder->async = async;
    decoder->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_JSONDECODER_CREATE, decoder->uid, decoder, async, source.obj,
            max_encoding_size);
    decoder->source = source;
    decoder->buffer = make_byte_array(max_encoding_size);
    decoder->eof = false;
    return decoder;
}

static ssize_t read_frame(void *obj, void *buf, size_t count)
{
    bytestream_1 *stream = obj;
    return bytestream_1_read(*stream, buf, count);
}

FSTRACE_DECL(ASYNC_JSONDECODER_RECEIVE_SYNTAX_ERROR, "UID=%64u");
FSTRACE_DECL(ASYNC_JSONDECODER_RECEIVE_INPUT_DUMP, "UID=%64u TEXT=%A");

static json_thing_t *do_receive(jsondecoder_t *decoder)
{
    if (decoder->eof) {
        errno = 0; /* EOF */
        return NULL;
    }
    for (;;) {
        ssize_t count = byte_array_append_stream(decoder->buffer, read_frame,
                                                 &decoder->source, 1024);
        if (count < 0 && errno == ENOSPC) {
            char c;
            count = bytestream_1_read(decoder->source, &c, 1);
            if (count > 0) {
                errno = ENOSPC;
                return NULL;
            }
        }
        if (count < 0)
            return NULL;
        if (!count) {
            decoder->eof = true;
            break;
        }
    }
    const char *buffer = byte_array_data(decoder->buffer);
    size_t amount = byte_array_size(decoder->buffer);
    FSTRACE(ASYNC_JSONDECODER_RECEIVE_INPUT_DUMP, decoder->uid, buffer, amount);
    json_thing_t *thing = json_utf8_decode(buffer, amount);
    if (!thing) {
        FSTRACE(ASYNC_JSONDECODER_RECEIVE_SYNTAX_ERROR, decoder->uid);
        errno = EILSEQ;
        return NULL;
    }
    return thing;
}

FSTRACE_DECL(ASYNC_JSONDECODER_RECEIVE, "UID=%64u THING=%p ERRNO=%e");

json_thing_t *jsondecoder_receive(jsondecoder_t *decoder)
{
    json_thing_t *thing = do_receive(decoder);
    FSTRACE(ASYNC_JSONDECODER_RECEIVE, decoder->uid, thing);
    return thing;
}

FSTRACE_DECL(ASYNC_JSONDECODER_CLOSE_FRAME, "UID=%64u");

void jsondecoder_close(jsondecoder_t *decoder)
{
    FSTRACE(ASYNC_JSONDECODER_CLOSE_FRAME, decoder->uid);
    assert(decoder->async);
    bytestream_1_close(decoder->source);
    destroy_byte_array(decoder->buffer);
    async_wound(decoder->async, decoder);
    decoder->async = NULL;
}

FSTRACE_DECL(ASYNC_JSONDECODER_REGISTER_FRAME, "UID=%64u OBJ=%p ACT=%p");

void jsondecoder_register_callback(jsondecoder_t *decoder, action_1 action)
{
    FSTRACE(ASYNC_JSONDECODER_REGISTER_FRAME, decoder->uid, action.obj,
            action.act);
    bytestream_1_register_callback(decoder->source, action);
}

FSTRACE_DECL(ASYNC_JSONDECODER_UNREGISTER_FRAME, "UID=%64u");

void jsondecoder_unregister_callback(jsondecoder_t *decoder)
{
    FSTRACE(ASYNC_JSONDECODER_UNREGISTER_FRAME, decoder->uid);
    bytestream_1_unregister_callback(decoder->source);
}
