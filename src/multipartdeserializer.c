#include "multipartdeserializer.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

#include <fsdyn/charstr.h>
#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "async_version.h"
#include "deserializer.h"
#include "multipartdecoder.h"

struct multipartdeserializer {
    async_t *async;
    uint64_t uid;
    deserializer_t *deserializer;
    char *boundary;
    bool first_part;
};

static bytestream_2 open_decoder(void *obj, bytestream_1 source)
{
    multipartdeserializer_t *des = obj;
    multipartdecoder_t *decoder =
        multipart_decode(des->async, source, des->boundary, des->first_part);
    des->first_part = false;
    return multipartdecoder_as_bytestream_2(decoder);
}

FSTRACE_DECL(ASYNC_MULTIPARTDESERIALIZER_CREATE,
             "UID=%64u PTR=%p ASYNC=%p SOURCE=%p");

multipartdeserializer_t *open_multipartdeserializer(async_t *async,
                                                    bytestream_1 source,
                                                    const char *boundary)
{
    multipartdeserializer_t *des = fsalloc(sizeof *des);
    des->async = async;
    des->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_MULTIPARTDESERIALIZER_CREATE, des->uid, des, async,
            source.obj);
    des->boundary = charstr_dupstr(boundary);
    des->first_part = true;
    des->deserializer = open_deserializer(async, source, open_decoder, des);
    return des;
}

FSTRACE_DECL(ASYNC_MULTIPARTDESERIALIZER_RECEIVE, "UID=%64u FRAME=%p ERRNO=%e");

bytestream_1 *multipartdeserializer_receive(multipartdeserializer_t *des)
{
    bytestream_1 *frame = deserializer_receive(des->deserializer);
    FSTRACE(ASYNC_MULTIPARTDESERIALIZER_RECEIVE, des->uid, frame);
    return frame;
}

static void *_receive(void *obj)
{
    return multipartdeserializer_receive(obj);
}

FSTRACE_DECL(ASYNC_MULTIPARTDESERIALIZER_CLOSE, "UID=%64u");

void multipartdeserializer_close(multipartdeserializer_t *des)
{
    FSTRACE(ASYNC_MULTIPARTDESERIALIZER_CLOSE, des->uid);
    fsfree(des->boundary);
    deserializer_close(des->deserializer);
    async_wound(des->async, des);
}

static void _close(void *obj)
{
    multipartdeserializer_close(obj);
}

FSTRACE_DECL(ASYNC_MULTIPARTDESERIALIZER_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void multipartdeserializer_register_callback(multipartdeserializer_t *des,
                                             action_1 action)
{
    FSTRACE(ASYNC_MULTIPARTDESERIALIZER_REGISTER, des->uid, action.obj,
            action.act);
    deserializer_register_callback(des->deserializer, action);
}

static void _register_callback(void *obj, action_1 action)
{
    multipartdeserializer_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_MULTIPARTDESERIALIZER_UNREGISTER, "UID=%64u");

void multipartdeserializer_unregister_callback(multipartdeserializer_t *des)
{
    FSTRACE(ASYNC_MULTIPARTDESERIALIZER_UNREGISTER, des->uid);
    multipartdeserializer_register_callback(des, NULL_ACTION_1);
}

static void _unregister_callback(void *obj)
{
    multipartdeserializer_unregister_callback(obj);
}

static const struct yield_1_vt multipartdeserializer_vt = {
    .receive = _receive,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

yield_1 multipartdeserializer_as_yield_1(multipartdeserializer_t *deserializer)
{
    return (yield_1) { deserializer, &multipartdeserializer_vt };
}
