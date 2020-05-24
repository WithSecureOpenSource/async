#ifndef __NAIVEENCODER__
#define __NAIVEENCODER__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct naiveencoder naiveencoder_t;

/*
 * Encode the underlying source stream by marking the end with a
 * terminator byte and (optionally) escaping special bytes. If escaping
 * is not needed, specify the terminator as the escape byte.
 */
naiveencoder_t *naive_encode(async_t *async, bytestream_1 source,
                             uint8_t terminator, uint8_t escape);

bytestream_1 naiveencoder_as_bytestream_1(naiveencoder_t *encoder);
ssize_t naiveencoder_read(naiveencoder_t *encoder, void *buf, size_t count);
void naiveencoder_close(naiveencoder_t *encoder);
void naiveencoder_register_callback(naiveencoder_t *encoder, action_1 action);
void naiveencoder_unregister_callback(naiveencoder_t *encoder);

#ifdef __cplusplus
}
#endif

#endif
