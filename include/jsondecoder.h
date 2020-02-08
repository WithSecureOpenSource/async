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
#endif

#endif
