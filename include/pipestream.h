#ifndef __PIPESTREAM__
#define __PIPESTREAM__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pipestream pipestream_t;

/*
 * Read an open file descriptor as a nonblocking stream. Suitable for
 * pipes and pipelike file descriptors.
 */
pipestream_t *open_pipestream(async_t *async, int fd);

bytestream_1 pipestream_as_bytestream_1(pipestream_t *pipestr);
ssize_t pipestream_read(pipestream_t *pipestr, void *buf, size_t count);
void pipestream_close(pipestream_t *pipestr);
void pipestream_register_callback(pipestream_t *pipestr, action_1 action);
void pipestream_unregister_callback(pipestream_t *pipestr);

#ifdef __cplusplus
}
#endif

#endif
