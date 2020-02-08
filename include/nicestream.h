#ifndef __NICESTREAM__
#define __NICESTREAM__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nicestream nicestream_t;

/*
 * Open a stream wrapper that prevents the underlying stream from
 * monopolizing the scheduler. The returned stream yields whenever the
 * given burst size has been relayed.
 */
nicestream_t *make_nice(async_t *async, bytestream_1 stream, size_t max_burst);

bytestream_1 nicestream_as_bytestream_1(nicestream_t *nice);
ssize_t nicestream_read(nicestream_t *nice, void *buf, size_t count);
void nicestream_close(nicestream_t *nice);
void nicestream_register_callback(nicestream_t *nice, action_1 action);
void nicestream_unregister_callback(nicestream_t *nice);

#ifdef __cplusplus
}
#endif

#endif
