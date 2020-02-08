#ifndef __BYTESTREAM_2__
#define __BYTESTREAM_2__

#include "action_1.h"
#include "async.h"

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *obj;
    const struct bytestream_2_vt *vt;
} bytestream_2;

struct bytestream_2_vt {
    ssize_t (*read)(void *obj, void *buf, size_t count);
    void (*close)(void *obj);
    void (*register_callback)(void *obj, action_1 action);
    void (*unregister_callback)(void *obj);
    ssize_t (*remaining)(void *obj);
    /* These methods return the leftover bytes that the stream has
     * read from the underlying stream but not consumed after read has
     * returned 0. The return value is undefined if the methods are
     * called before read returns 0. */
    ssize_t (*leftover_size)(void *obj);
    void *(*leftover_bytes)(void *obj);
};

static inline
ssize_t bytestream_2_read(bytestream_2 stream, void *buf, size_t count)
{
    return stream.vt->read(stream.obj, buf, count);
}

static inline
void bytestream_2_close(bytestream_2 stream)
{
    stream.vt->close(stream.obj);
}

static inline
void bytestream_2_register_callback(bytestream_2 stream, action_1 action)
{
    stream.vt->register_callback(stream.obj, action);
}

static inline
void bytestream_2_unregister_callback(bytestream_2 stream)
{
    stream.vt->unregister_callback(stream.obj);
}

static inline
ssize_t bytestream_2_remaining(bytestream_2 stream)
{
    return stream.vt->remaining(stream.obj);
}

static inline
ssize_t bytestream_2_leftover_size(bytestream_2 stream)
{
    return stream.vt->leftover_size(stream.obj);
}

static inline
void *bytestream_2_leftover_bytes(bytestream_2 stream)
{
    return stream.vt->leftover_bytes(stream.obj);
}

static inline
bytestream_1 bytestream_2_as_bytestream_1(bytestream_2 stream)
{
    struct bytestream_1_vt *vt = (struct bytestream_1_vt *) stream.vt;
    return (bytestream_1) { stream.obj, vt };
}

#ifdef __cplusplus
}
#endif

#endif
