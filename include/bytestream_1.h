#ifndef __BYTESTREAM__
#define __BYTESTREAM__

#include <sys/types.h>

#include "action_1.h"
#include "async.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The bytestream_1 interface provides a readable, nonblocking pipe
 * abstraction. See also bytestream_2. */
typedef struct {
    void *obj;
    const struct bytestream_1_vt *vt;
} bytestream_1;

struct bytestream_1_vt {
    /* The read method works analogously to the read(2) system call.
     * In particular, a negative value is returned and errno is set in
     * error situations. The method should never block. The method
     * must not be called after the close method has been called. */
    ssize_t (*read)(void *obj, void *buf, size_t count);

    /* The close method works analogously to the close(2) system call
     * with the exception that it can never block or fail. The
     * underlying resources are released instantanously or in a
     * delayed fashion. */
    void (*close)(void *obj);

    /* A callback action can be registered with the register_callback
     * method. The callback suggests the read method should be called.
     * The callback function may be called "posthumously" so it must
     * additionally make sure read is not called after close is
     * called.
     *
     * In most environments, edge-triggered semantics are followed,
     * which means the owner of the bytestream object is responsible
     * for calling read without a callback. A callback is guaranteed
     * only after read returns a negative value with errno == EAGAIN.
     *
     * Calling register_callback again replaces the previous callback
     * action. Initially, no callback is registered.
     *
     * The method must not be called after the close method has been
     * called. */
    void (*register_callback)(void *obj, action_1 action);

    /* A callback action is unregistered with the unregister_callback
     * method. Leftover callback that were triggered previously
     * actions might still take place. The method can be called even
     * when no callback is registered but it must not be called after
     * the close method has been called. */
    void (*unregister_callback)(void *obj);
};

static inline ssize_t bytestream_1_read(bytestream_1 stream, void *buf,
                                        size_t count)
{
    return stream.vt->read(stream.obj, buf, count);
}

static inline void bytestream_1_close(bytestream_1 stream)
{
    stream.vt->close(stream.obj);
}

/* bytestream_1_close_relaxed() schedules a call to
 * bytestream_1_close() at the first opportunity from the async main
 * loop. */
void bytestream_1_close_relaxed(async_t *async, bytestream_1 stream);

static inline void bytestream_1_register_callback(bytestream_1 stream,
                                                  action_1 action)
{
    stream.vt->register_callback(stream.obj, action);
}

static inline void bytestream_1_unregister_callback(bytestream_1 stream)
{
    stream.vt->unregister_callback(stream.obj);
}

#ifdef __cplusplus
}
#endif

#endif
