#ifndef __BLOBSTREAM__
#define __BLOBSTREAM__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct blobstream blobstream_t;

/*
 * Return a bytestream that returns the bytes of the given blob. The
 * blob must stay in existence until the stream is closed.
 */
blobstream_t *open_blobstream(async_t *async, const void *blob, size_t count);

/*
 * Like open_blobstream() but blob is copied by copy_blobstream().
 */
blobstream_t *copy_blobstream(async_t *async, const void *blob, size_t count);

/*
 * Like open_blobstream() but close_action is performed when the stream
 * is closed.
 */
blobstream_t *adopt_blobstream(async_t *async, const void *blob, size_t count,
                               action_1 close_action);

bytestream_1 blobstream_as_bytestream_1(blobstream_t *blobstr);
size_t blobstream_remaining(blobstream_t *blobstr);
ssize_t blobstream_read(blobstream_t *blobstr, void *buf, size_t count);
void blobstream_close(blobstream_t *blobstr);
void blobstream_register_callback(blobstream_t *blobstr, action_1 action);
void blobstream_unregister_callback(blobstream_t *blobstr);

#ifdef __cplusplus
}
#endif

#endif
