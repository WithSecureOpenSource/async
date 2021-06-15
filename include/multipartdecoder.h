#ifndef __MULTIPARTDECODER__
#define __MULTIPARTDECODER__

#include <stdbool.h>

#include "async.h"
#include "bytestream_1.h"
#include "bytestream_2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct multipartdecoder multipartdecoder_t;

/*
 * Decode one part of the given RFC 2046 multipart body stream.
 */
multipartdecoder_t *multipart_decode(async_t *async, bytestream_1 source,
                                     const char *boundary, bool first_part);

bytestream_2 multipartdecoder_as_bytestream_2(multipartdecoder_t *decoder);
ssize_t multipartdecoder_read(multipartdecoder_t *decoder, void *buf,
                              size_t count);

void multipartdecoder_close(multipartdecoder_t *decoder);
void multipartdecoder_register_callback(multipartdecoder_t *decoder,
                                        action_1 action);
void multipartdecoder_unregister_callback(multipartdecoder_t *decoder);

size_t multipartdecoder_leftover_size(multipartdecoder_t *decoder);
void *multipartdecoder_leftover_bytes(multipartdecoder_t *decoder);

#ifdef __cplusplus
}
#endif

#endif
