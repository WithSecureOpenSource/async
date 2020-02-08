#ifndef __CLOBBERSTREAM__
#define __CLOBBERSTREAM__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct clobberstream clobberstream_t;

/*
 * Open a stream wrapper that XORs the content of the underlying stream
 * with the given mask starting at the given offset. The LSB is XORed
 * with the lowest offset (little-endian semantics).
 */
clobberstream_t *clobber(async_t *async, bytestream_1 stream, size_t offset,
                         uint64_t mask);

bytestream_1 clobberstream_as_bytestream_1(clobberstream_t *clstr);
ssize_t clobberstream_read(clobberstream_t *clstr, void *buf, size_t count);
void clobberstream_close(clobberstream_t *clstr);
void clobberstream_register_callback(clobberstream_t *clstr, action_1 action);
void clobberstream_unregister_callback(clobberstream_t *clstr);

#ifdef __cplusplus
}
#endif

#endif
