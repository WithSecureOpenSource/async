#include "concatstream.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <fsdyn/fsalloc.h>

#include "async.h"
#include "async_version.h"
#include "queuestream.h"

/* No need for a separate type. Just type cast queuestream_t
 * everywhere. */

ssize_t concatstream_read(concatstream_t *conc, void *buf, size_t count)
{
    return queuestream_read((queuestream_t *) conc, buf, count);
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return concatstream_read(obj, buf, count);
}

void concatstream_close(concatstream_t *conc)
{
    queuestream_close((queuestream_t *) conc);
}

static void _close(void *obj)
{
    concatstream_close(obj);
}

void concatstream_register_callback(concatstream_t *conc, action_1 action)
{
    queuestream_register_callback((queuestream_t *) conc, action);
}

static void _register_callback(void *obj, action_1 action)
{
    concatstream_register_callback(obj, action);
}

void concatstream_unregister_callback(concatstream_t *conc)
{
    queuestream_unregister_callback((queuestream_t *) conc);
}

static void _unregister_callback(void *obj)
{
    concatstream_unregister_callback(obj);
}

static const struct bytestream_1_vt concatstream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 concatstream_as_bytestream_1(concatstream_t *conc)
{
    return (bytestream_1) { conc, &concatstream_vt };
}

concatstream_t *concatenate_streams(async_t *async, bytestream_1 streams[],
                                    size_t count)
{
    queuestream_t *qstr = make_queuestream(async);
    size_t i;
    for (i = 0; i < count; i++)
        queuestream_enqueue(qstr, streams[i]);
    queuestream_terminate(qstr);
    return (concatstream_t *) qstr;
}

concatstream_t *concatenate_two_streams(async_t *async, bytestream_1 first,
                                        bytestream_1 second)
{
    bytestream_1 streams[] = {
        first,
        second,
    };
    return concatenate_streams(async, streams, 2);
}

concatstream_t *concatenate_three_streams(async_t *async, bytestream_1 first,
                                          bytestream_1 second,
                                          bytestream_1 third)
{
    bytestream_1 streams[] = {
        first,
        second,
        third,
    };
    return concatenate_streams(async, streams, 3);
}
