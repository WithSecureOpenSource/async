#ifndef __CONCATSTREAM__
#define __CONCATSTREAM__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct concatstream concatstream_t;

/*
 * Open a stream that is the concatenation of the given streams, in the
 * given order. The count may be zero.
 */
concatstream_t *concatenate_streams(async_t *async, bytestream_1 streams[],
                                    size_t count);

concatstream_t *concatenate_two_streams(async_t *async, bytestream_1 first,
                                        bytestream_1 second);

concatstream_t *concatenate_three_streams(async_t *async, bytestream_1 first,
                                          bytestream_1 second,
                                          bytestream_1 third);

bytestream_1 concatstream_as_bytestream_1(concatstream_t *conc);
ssize_t concatstream_read(concatstream_t *conc, void *buf, size_t count);
void concatstream_close(concatstream_t *conc);
void concatstream_register_callback(concatstream_t *conc, action_1 action);
void concatstream_unregister_callback(concatstream_t *conc);

#ifdef __cplusplus
}
#endif

#endif
