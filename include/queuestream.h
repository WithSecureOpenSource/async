#ifndef __QUEUESTREAM__
#define __QUEUESTREAM__

#include <stdbool.h>

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct queuestream queuestream_t;

/* Open a stream that can append (and prepend) new bytestreams. If the
 * queue is empty, the queue stream acts like drystream_t unless/until
 * queuestream_terminate() is called.
 *
 * The returned queuestream is "released." In other words, a
 * queuestream_close() call from the consumer will deallocate the
 * queuestream_t object instantly. */
queuestream_t *make_queuestream(async_t *async);

/* Open a reference-counted queuestream. A queuestream opened with
 * make_questream() gets deallocated instantly when its consumer calls
 * queuestream_close(). That "electric" behaviour is especially
 * problematic for the queuestream, which has a second user: the
 * producer.
 *
 * This function returns a queuestream_t object that will not be
 * deallocated before both the consumer and the producer relinquish
 * the object. To relinquish it, the consumer calls
 * queuestream_close(), and the producer calls
 * queuestream_release(). */
queuestream_t *make_relaxed_queuestream(async_t *async);

/* Return true iff the consumer has called queuestream_close() on the
 * queuestream. */
bool queuestream_closed(queuestream_t *qstr);

/* Release a queuestream returned by make_relaxed_queuestream(). The
 * function is called by the producer to indicate it will not access
 * the queuestream again. */
void queuestream_release(queuestream_t *qstr);

/* Append a bytestream. If the queuestream has been closed but not
 * released, the function produces no effect. */
void queuestream_enqueue(queuestream_t *qstr, bytestream_1 stream);

/* Prepend a bytestream. If the queuestream has been closed but not
 * released, the function produces no effect. */
void queuestream_push(queuestream_t *qstr, bytestream_1 stream);

/* Append a byte sequence. If the queuestream has been closed but not
 * released, the function produces no effect. */
void queuestream_enqueue_bytes(queuestream_t *qstr, const void *blob,
                               size_t count);

/* Prepend a byte sequence. If the queuestream has been closed but not
 * released, the function produces no effect. */
void queuestream_push_bytes(queuestream_t *qstr, const void *blob,
                            size_t count);

/* Indicate that once the queuestream is exhausted, queuestream_read()
 * should return 0 (EOF) instead of -1 with EAGAIN. If the queuestream
 * has been closed but not released, the function produces no effect.
 *
 * Note that it is valid to call queuestream_push() even after
 * queuestream_terminate() has been called, although that should be done
 * before queuestream_read() has returned 0 for an EOF. */
void queuestream_terminate(queuestream_t *qstr);

bytestream_1 queuestream_as_bytestream_1(queuestream_t *qstr);
ssize_t queuestream_read(queuestream_t *qstr, void *buf, size_t count);
void queuestream_close(queuestream_t *qstr);
void queuestream_register_callback(queuestream_t *qstr, action_1 action);
void queuestream_unregister_callback(queuestream_t *qstr);

#ifdef __cplusplus
}
#endif

#endif
