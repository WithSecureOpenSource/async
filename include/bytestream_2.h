#ifndef __BYTESTREAM_2__
#define __BYTESTREAM_2__

#include <sys/types.h>

#include "action_1.h"
#include "async.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The bytestream_1 interface provides a readable, nonblocking pipe
 * abstraction. */
typedef struct {
    void *obj;
    const struct bytestream_2_vt *vt;
} bytestream_2;

struct bytestream_2_vt {
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

    /* Return the number of bytes remaining to be read until EOF is
     * reached. If the information is not available, a negative value
     * is returned and errno == ENOTSUP. Other errno values are
     * possible. The method must not be called after the close method
     * has been called. */
    ssize_t (*remaining)(void *obj);

    /* These methods return the leftover bytes that the stream has
     * read from the underlying stream but not consumed after read has
     * returned 0. The return value is undefined if the methods are
     * called before read returns 0. The methods must not be called
     * after the close method has been called. */
    ssize_t (*leftover_size)(void *obj);
    void *(*leftover_bytes)(void *obj);
};

static inline ssize_t bytestream_2_read(bytestream_2 stream, void *buf,
                                        size_t count)
{
    return stream.vt->read(stream.obj, buf, count);
}

static inline void bytestream_2_close(bytestream_2 stream)
{
    stream.vt->close(stream.obj);
}

static inline void bytestream_2_register_callback(bytestream_2 stream,
                                                  action_1 action)
{
    stream.vt->register_callback(stream.obj, action);
}

static inline void bytestream_2_unregister_callback(bytestream_2 stream)
{
    stream.vt->unregister_callback(stream.obj);
}

static inline ssize_t bytestream_2_remaining(bytestream_2 stream)
{
    return stream.vt->remaining(stream.obj);
}

static inline ssize_t bytestream_2_leftover_size(bytestream_2 stream)
{
    return stream.vt->leftover_size(stream.obj);
}

static inline void *bytestream_2_leftover_bytes(bytestream_2 stream)
{
    return stream.vt->leftover_bytes(stream.obj);
}

static inline bytestream_1 bytestream_2_as_bytestream_1(bytestream_2 stream)
{
    struct bytestream_1_vt *vt = (struct bytestream_1_vt *) stream.vt;
    return (bytestream_1) { stream.obj, vt };
}

#ifdef __cplusplus
}
#endif

#endif
