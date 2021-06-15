#ifndef _PAUSESTREAM_H_
#define _PAUSESTREAM_H_

#include <sys/types.h>

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

/* This mysterious line fails the compilation if sizeof(off_t) < 8
 * bytes. Use _FILE_OFFSET_BITS=64 on 32-bit Linux systems. */
extern void PAUSESTREAM_OFF_T_CHECK(char[sizeof(off_t) - 8]);

typedef struct pausestream pausestream_t;

typedef struct {
    void *obj;
    off_t (*limit)(void *obj);
} pausestream_limit_cb_1;

/*
 * Like blockingstream, except only reads up to a limit before
 * drying up (until limit is raised).
 *
 * The stream obtains the current limit by calling a callback.
 * After the stream is created, it stays paused at position 0 until
 * the callback is set. Callback should return the current limit,
 * or -1 to allow streaming until end of input.
 */

pausestream_t *open_pausestream(async_t *async, int fd);
void pausestream_set_limit_callback(pausestream_t *pausestr,
                                    pausestream_limit_cb_1 limit_cb);
bytestream_1 pausestream_as_bytestream_1(pausestream_t *pausestr);
ssize_t pausestream_read(pausestream_t *pausestr, void *buf, size_t count);
void pausestream_close(pausestream_t *pausestr);

#ifdef __cplusplus
}
#endif

#endif // _PAUSESTREAM_H_
