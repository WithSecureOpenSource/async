#include <errno.h>
#include <assert.h>
#include <fstrace.h>
#include <fsdyn/fsalloc.h>
#include "jsondecoder.h"
#include "reservoir.h"
#include "async_version.h"

struct jsondecoder {
    async_t *async;
    uint64_t uid;
    reservoir_t *source;
};

FSTRACE_DECL(ASYNC_JSONDECODER_CREATE,
             "UID=%64u PTR=%p ASYNC=%p SOURCE=%p MAX-SIZE=%z");

jsondecoder_t *open_jsondecoder(async_t *async, bytestream_1 source,
                                size_t max_encoding_size)
{
    jsondecoder_t *decoder = fsalloc(sizeof *decoder);
    decoder->async = async;
    decoder->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_JSONDECODER_CREATE,
            decoder->uid, decoder, async, source.obj, max_encoding_size);
    decoder->source = open_reservoir(async, max_encoding_size, source);
    return decoder;
}

FSTRACE_DECL(ASYNC_JSONDECODER_RECEIVE_SYNTAX_ERROR, "UID=%64u");
FSTRACE_DECL(ASYNC_JSONDECODER_RECEIVE_INPUT_DUMP, "UID=%64u TEXT=%A");

static json_thing_t *do_receive(jsondecoder_t *decoder)
{
    if (!decoder->source) {
        errno = 0;              /* EOF */
        return NULL;
    }
    if (!reservoir_fill(decoder->source))
        return NULL;
    size_t amount = reservoir_amount(decoder->source);
    size_t remaining = amount;
    uint8_t *buffer = fsalloc(remaining);
    uint8_t *p = buffer;
    while (remaining) {
        ssize_t count = reservoir_read(decoder->source, p, remaining);
        assert(count > 0);
        p += count;
        remaining -= count;
    }
    FSTRACE(ASYNC_JSONDECODER_RECEIVE_INPUT_DUMP, decoder->uid, buffer, amount);
    json_thing_t *thing = json_utf8_decode(buffer, amount);
    fsfree(buffer);
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
    reservoir_close(decoder->source);
    async_wound(decoder->async, decoder);
    decoder->async = NULL;
}

FSTRACE_DECL(ASYNC_JSONDECODER_REGISTER_FRAME, "UID=%64u OBJ=%p ACT=%p");

void jsondecoder_register_callback(jsondecoder_t *decoder, action_1 action)
{
    FSTRACE(ASYNC_JSONDECODER_REGISTER_FRAME,
            decoder->uid, action.obj, action.act);
    reservoir_register_callback(decoder->source, action);
}

FSTRACE_DECL(ASYNC_JSONDECODER_UNREGISTER_FRAME, "UID=%64u");

void jsondecoder_unregister_callback(jsondecoder_t *decoder)
{
    FSTRACE(ASYNC_JSONDECODER_UNREGISTER_FRAME, decoder->uid);
    reservoir_unregister_callback(decoder->source);
}
