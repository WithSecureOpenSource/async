#ifndef __JSONDECODER__
#define __JSONDECODER__

#include <encjson.h>

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jsondecoder jsondecoder_t;

/* Decode a single JSON thing out of a byte stream. */
jsondecoder_t *open_jsondecoder(async_t *async, bytestream_1 source,
                                size_t max_encoding_size);

/* The returned JSON thing must be disposed of by the user. */
json_thing_t *jsondecoder_receive(jsondecoder_t *decoder);

/* Closing the JSON decoder closes the source stream as well. */
void jsondecoder_close(jsondecoder_t *decoder);
void jsondecoder_register_callback(jsondecoder_t *decoder, action_1 action);
void jsondecoder_unregister_callback(jsondecoder_t *decoder);

#ifdef __cplusplus
}

#include <functional>
#include <memory>

namespace fsecure {
namespace async {

// std::unique_ptr for jsondecoder_t with custom deleter.
using JsondecoderPtr =
    std::unique_ptr<jsondecoder_t, std::function<void(jsondecoder_t *)>>;

// Create JsondecoderPtr that takes ownership of the provided jsondecoder_t.
// Pass nullptr to create an instance which doesn't contain any jsondecoder_t
// object.
inline JsondecoderPtr make_jsondecoder_ptr(jsondecoder_t *decoder)
{
    return { decoder, jsondecoder_close };
}

} // namespace async
} // namespace fsecure

#endif

#endif
