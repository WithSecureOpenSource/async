#ifndef __CHUNKENCODER__
#define __CHUNKENCODER__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chunkencoder chunkencoder_t;

/*
 * Split the underlying stream into chunks of the given size (or
 * smaller). Frame the chunks with the chunked encoding format.
 * Equivalent with chunk_encode_2(..., CHUNKENCODER_SIMPLE).
 */
chunkencoder_t *chunk_encode(async_t *async, bytestream_1 stream,
                             size_t max_chunk_size);

typedef enum {
    CHUNKENCODER_SIMPLE,                  /* terminate with "0\r\n\r\n" */
    CHUNKENCODER_STOP_AT_TRAILER,         /* terminate with "0\r\n"     */
    CHUNKENCODER_STOP_AT_FINAL_EXTENSIONS /* terminate with "0"         */
} chunkencoder_termination_t;

chunkencoder_t *chunk_encode_2(async_t *async, bytestream_1 stream,
                               size_t max_chunk_size,
                               chunkencoder_termination_t termination);

bytestream_1 chunkencoder_as_bytestream_1(chunkencoder_t *encoder);
ssize_t chunkencoder_read(chunkencoder_t *encoder, void *buf, size_t count);
void chunkencoder_close(chunkencoder_t *encoder);
void chunkencoder_register_callback(chunkencoder_t *encoder, action_1 action);
void chunkencoder_unregister_callback(chunkencoder_t *encoder);

#ifdef __cplusplus
}
#endif

#endif
