#ifndef __BLOCKINGSTREAM__
#define __BLOCKINGSTREAM__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct blockingstream blockingstream_t;

/*
 * Read an open file as a stream. The read operation might cause the
 * process to block as data is being retrieved from the physical medium.
 */
blockingstream_t *open_blockingstream(async_t *async, int fd);

bytestream_1 blockingstream_as_bytestream_1(blockingstream_t *blockingstr);
ssize_t blockingstream_read(blockingstream_t *blockingstr, void *buf,
                            size_t count);
void blockingstream_close(blockingstream_t *blockingstr);
void blockingstream_register_callback(blockingstream_t *blockingstr,
                                      action_1 action);
void blockingstream_unregister_callback(blockingstream_t *blockingstr);

#ifdef __cplusplus
}
#endif

#endif
