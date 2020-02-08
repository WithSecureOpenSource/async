#ifndef __BYTESTREAM__
#define __BYTESTREAM__

#include <sys/types.h>
#include "async.h"
#include "action_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *obj;
    const struct bytestream_1_vt *vt;
} bytestream_1;

struct bytestream_1_vt {
    ssize_t (*read)(void *obj, void *buf, size_t count);
    void (*close)(void *obj);
    void (*register_callback)(void *obj, action_1 action);
    void (*unregister_callback)(void *obj);
};

static inline
ssize_t bytestream_1_read(bytestream_1 stream, void *buf, size_t count)
{
    return stream.vt->read(stream.obj, buf, count);
}

static inline
void bytestream_1_close(bytestream_1 stream)
{
    stream.vt->close(stream.obj);
}

void bytestream_1_close_relaxed(async_t *async, bytestream_1 stream);

static inline
void bytestream_1_register_callback(bytestream_1 stream, action_1 action)
{
    stream.vt->register_callback(stream.obj, action);
}

static inline
void bytestream_1_unregister_callback(bytestream_1 stream)
{
    stream.vt->unregister_callback(stream.obj);
}

#ifdef __cplusplus
}
#endif

#endif
