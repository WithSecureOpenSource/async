#ifndef __ASYNC_YIELD__
#define __ASYNC_YIELD__

#include "action_1.h"
#include "async.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A yield is an input sequence of arbitrary data objects, typically
 * driven by I/O events. */

typedef struct {
    void *obj; /* The implementation of the yield object. */
    const struct yield_1_vt *vt;
} yield_1;

struct yield_1_vt {
    /* Return an object from the yield.
     *
     * NULL is returned in the following situations:
     *
     *  1. The yield has been exhausted cleanly. In this case, errno == 0.
     *
     *  2. The yield does not have anything available at the moment. In
     *     this case, errno == EAGAIN.
     *
     *  3. An error has occurred. The error is indicated by errno. */
    void *(*receive)(void *obj);

    /* A yield can be closed (once) at any time. The yield must be
     * closed eventually to free up the associated resources. */
    void (*close)(void *obj);

    void (*register_callback)(void *obj, action_1 action);
    void (*unregister_callback)(void *obj);
};

static inline void *yield_1_receive(yield_1 yield)
{
    return yield.vt->receive(yield.obj);
}

static inline void yield_1_close(yield_1 yield)
{
    yield.vt->close(yield.obj);
}

void yield_1_close_relaxed(async_t *async, yield_1 yield);

static inline void yield_1_register_callback(yield_1 yield, action_1 action)
{
    yield.vt->register_callback(yield.obj, action);
}

static inline void yield_1_unregister_callback(yield_1 yield)
{
    yield.vt->unregister_callback(yield.obj);
}

#ifdef __cplusplus
}
#endif

#endif
