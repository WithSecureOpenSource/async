#ifndef __ERRORSTREAM__
#define __ERRORSTREAM__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct errorstream errorstream_t;

/*
 * An error stream's read method always returns -1 with the given errno
 * value.
 */
errorstream_t *make_errorstream(async_t *async, int err);

bytestream_1 errorstream_as_bytestream_1(errorstream_t *estr);
ssize_t errorstream_read(errorstream_t *estr, void *buf, size_t count);
void errorstream_close(errorstream_t *estr);
void errorstream_register_callback(errorstream_t *estr, action_1 action);
void errorstream_unregister_callback(errorstream_t *estr);

#ifdef __cplusplus
}
#endif

#endif
