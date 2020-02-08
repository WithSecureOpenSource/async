#ifndef __CHUNKFRAMER__
#define __CHUNKFRAMER__

#include "async.h"
#include "bytestream_1.h"
#include "yield_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chunkframer chunkframer_t;

/* Break the given source byte stream into frames, which are themselves
 * byte streams.
 *
 * The frames in source must be encoded using the HTTP Chunked Encoding
 * format. */
chunkframer_t *open_chunkframer(async_t *async, bytestream_1 source);

yield_1 chunkframer_as_yield_1(chunkframer_t *framer);

/* The returned frame is normally closed by the user. However, the
 * framer retains ownership to the frame; if the frame is still open
 * when the framer is closed, the framer closes it. */
bytestream_1 *chunkframer_receive(chunkframer_t *framer);

/* Closing the chunk framer closes the source stream as well. */
void chunkframer_close(chunkframer_t *framer);
void chunkframer_register_callback(chunkframer_t *framer, action_1 action);
void chunkframer_unregister_callback(chunkframer_t *framer);

#ifdef __cplusplus
}
#endif

#endif
