#ifndef __JSONENCODER__
#define __JSONENCODER__

#include <encjson.h>

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jsonencoder jsonencoder_t;

/*
 * Encode the given JSON thing into a byte stream.
 *
 * The given JSON thing is left intact.
 */
jsonencoder_t *json_encode(async_t *async, json_thing_t *thing);

/* Return the size of the encoding */
size_t jsonencoder_size(jsonencoder_t *encoder);

bytestream_1 jsonencoder_as_bytestream_1(jsonencoder_t *encoder);
ssize_t jsonencoder_read(jsonencoder_t *encoder, void *buf, size_t count);
void jsonencoder_close(jsonencoder_t *encoder);
void jsonencoder_register_callback(jsonencoder_t *encoder, action_1 action);
void jsonencoder_unregister_callback(jsonencoder_t *encoder);

#ifdef __cplusplus
}
#endif

#endif
