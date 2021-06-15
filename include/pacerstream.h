#ifndef __PACERSTREAM__
#define __PACERSTREAM__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pacerstream pacerstream_t;

/*
 * Open a rate-limited stream.
 */
pacerstream_t *pace_stream(async_t *async, bytestream_1 stream, double byterate,
                           size_t min_burst, size_t max_burst);

/*
 * Reset timers and counters. Try reading immediately after calling
 * pacerstream_reset().
 */
void pacerstream_reset(pacerstream_t *pacer);

bytestream_1 pacerstream_as_bytestream_1(pacerstream_t *pacer);
ssize_t pacerstream_read(pacerstream_t *pacer, void *buf, size_t count);
void pacerstream_close(pacerstream_t *pacer);
void pacerstream_register_callback(pacerstream_t *pacer, action_1 action);
void pacerstream_unregister_callback(pacerstream_t *pacer);

#ifdef __cplusplus
}
#endif

#endif
