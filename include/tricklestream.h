#ifndef __TRICKLESTREAM__
#define __TRICKLESTREAM__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tricklestream tricklestream_t;

/*
 * Wrap a byte stream. Slow down the transmission down to a single byte
 * per 'interval' (in seconds).
 */
tricklestream_t *open_tricklestream(async_t *async, bytestream_1 stream,
                                    double interval);

/*
 * Stop trickling. Let the bytes through as soon as they arrive. Try
 * reading immediately after calling tricklestream_release().
 */
void tricklestream_release(tricklestream_t *trickle);

bytestream_1 tricklestream_as_bytestream_1(tricklestream_t *trickle);
ssize_t tricklestream_read(tricklestream_t *trickle, void *buf, size_t count);
void tricklestream_close(tricklestream_t *trickle);
void tricklestream_register_callback(tricklestream_t *trickle, action_1 action);
void tricklestream_unregister_callback(tricklestream_t *trickle);

#ifdef __cplusplus
}
#endif

#endif
