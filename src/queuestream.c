#include "queuestream.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

#include <fsdyn/fsalloc.h>
#include <fsdyn/list.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"
#include "blobstream.h"

struct queuestream {
    async_t *async;
    uint64_t uid;
    int pending_errno; /* or 0 */
    list_t *queue;     /* of bytestream_1 */
    bool terminated, closed, released;
    action_1 notifier;
    bool notification_expected;
};

FSTRACE_DECL(ASYNC_QUEUESTREAM_CREATE, "UID=%64u PTR=%p ASYNC=%p");

queuestream_t *make_relaxed_queuestream(async_t *async)
{
    queuestream_t *qstr = fsalloc(sizeof *qstr);
    qstr->async = async;
    qstr->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_QUEUESTREAM_CREATE, qstr->uid, qstr, async);
    qstr->pending_errno = 0;
    qstr->terminated = qstr->released = qstr->closed = false;
    qstr->queue = make_list();
    qstr->notifier = NULL_ACTION_1;
    qstr->notification_expected = false;
    return qstr;
}

queuestream_t *make_queuestream(async_t *async)
{
    queuestream_t *qstr = make_relaxed_queuestream(async);
    queuestream_release(qstr);
    return qstr;
}

bool queuestream_closed(queuestream_t *qstr)
{
    return qstr->closed;
}

FSTRACE_DECL(ASYNC_QUEUESTREAM_RELEASE, "UID=%64u CLOSED=%b");

void queuestream_release(queuestream_t *qstr)
{
    FSTRACE(ASYNC_QUEUESTREAM_RELEASE, qstr->uid, qstr->closed);
    assert(!qstr->released);
    if (qstr->closed)
        async_wound(qstr->async, qstr);
    qstr->released = true;
}

static void notify(queuestream_t *qstr)
{
    if (!qstr->notification_expected)
        return;
    qstr->notification_expected = false;
    action_1_perf(qstr->notifier);
}

FSTRACE_DECL(ASYNC_QUEUESTREAM_ENQUEUE, "UID=%64u STREAM=%p");
FSTRACE_DECL(ASYNC_QUEUESTREAM_ENQUEUE_POSTHUMOUSLY, "UID=%64u STREAM=%p");

void queuestream_enqueue(queuestream_t *qstr, bytestream_1 stream)
{
    if (qstr->closed) {
        FSTRACE(ASYNC_QUEUESTREAM_ENQUEUE_POSTHUMOUSLY, qstr->uid, stream.obj);
        assert(!qstr->released);
        bytestream_1_close_relaxed(qstr->async, stream);
        return;
    }
    FSTRACE(ASYNC_QUEUESTREAM_ENQUEUE, qstr->uid, stream.obj);
    bytestream_1 *elemstream = fsalloc(sizeof *elemstream);
    *elemstream = stream;
    list_append(qstr->queue, elemstream);
    action_1 callback = { qstr, (act_1) notify };
    bytestream_1_register_callback(stream, callback);
    async_execute(qstr->async, callback);
}

FSTRACE_DECL(ASYNC_QUEUESTREAM_PUSH, "UID=%64u STREAM=%p");
FSTRACE_DECL(ASYNC_QUEUESTREAM_PUSH_POSTHUMOUSLY, "UID=%64u STREAM=%p");

void queuestream_push(queuestream_t *qstr, bytestream_1 stream)
{
    if (qstr->closed) {
        FSTRACE(ASYNC_QUEUESTREAM_PUSH_POSTHUMOUSLY, qstr->uid, stream.obj);
        assert(!qstr->released);
        bytestream_1_close_relaxed(qstr->async, stream);
        return;
    }
    FSTRACE(ASYNC_QUEUESTREAM_PUSH, qstr->uid, stream.obj);
    assert(!qstr->closed || !qstr->released);
    bytestream_1 *elemstream = fsalloc(sizeof *elemstream);
    *elemstream = stream;
    list_prepend(qstr->queue, elemstream);
    action_1 callback = { qstr, (act_1) notify };
    bytestream_1_register_callback(stream, callback);
    async_execute(qstr->async, callback);
}

FSTRACE_DECL(ASYNC_QUEUESTREAM_ENQUEUE_BYTES, "UID=%64u DATA=%A");

void queuestream_enqueue_bytes(queuestream_t *qstr, const void *blob,
                               size_t count)
{
    FSTRACE(ASYNC_QUEUESTREAM_ENQUEUE_BYTES, qstr->uid, blob, count);
    blobstream_t *blobstr = copy_blobstream(qstr->async, blob, count);
    queuestream_enqueue(qstr, blobstream_as_bytestream_1(blobstr));
}

FSTRACE_DECL(ASYNC_QUEUESTREAM_PUSH_BYTES, "UID=%64u DATA=%A");

void queuestream_push_bytes(queuestream_t *qstr, const void *blob, size_t count)
{
    FSTRACE(ASYNC_QUEUESTREAM_PUSH_BYTES, qstr->uid, blob, count);
    blobstream_t *blobstr = copy_blobstream(qstr->async, blob, count);
    queuestream_push(qstr, blobstream_as_bytestream_1(blobstr));
}

FSTRACE_DECL(ASYNC_QUEUESTREAM_TERMINATE, "UID=%64u");
FSTRACE_DECL(ASYNC_QUEUESTREAM_TERMINATE_POSTHUMOUSLY, "UID=%64u");

void queuestream_terminate(queuestream_t *qstr)
{
    if (qstr->closed) {
        FSTRACE(ASYNC_QUEUESTREAM_TERMINATE_POSTHUMOUSLY, qstr->uid);
        assert(!qstr->released);
        return;
    }
    FSTRACE(ASYNC_QUEUESTREAM_TERMINATE, qstr->uid);
    qstr->terminated = true;
    action_1 callback = { qstr, (act_1) notify };
    async_execute(qstr->async, callback);
}

static ssize_t do_read(queuestream_t *qstr, void *buf, size_t count)
{
    if (qstr->pending_errno) {
        errno = qstr->pending_errno;
        qstr->pending_errno = 0;
        return -1;
    }
    if ((ssize_t) count < 0)
        count = (size_t) -1 >> 1;
    size_t cursor = 0;
    while (cursor < count && !list_empty(qstr->queue)) {
        list_elem_t *head_elem = list_get_first(qstr->queue);
        bytestream_1 *head = (bytestream_1 *) list_elem_get_value(head_elem);
        ssize_t n = bytestream_1_read(*head, buf + cursor, count - cursor);
        if (n < 0) {
            if (cursor == 0) {
                if (errno == EAGAIN)
                    qstr->notification_expected = true;
                return n;
            }
            if (errno != EAGAIN)
                qstr->pending_errno = errno;
            break;
        }
        if (n == 0) {
            bytestream_1_close(*head);
            fsfree(head);
            list_remove(qstr->queue, head_elem);
            continue;
        }
        cursor += n;
    }
    if (cursor > 0)
        return cursor;
    if (qstr->terminated)
        return 0;
    qstr->notification_expected = true;
    errno = EAGAIN;
    return -1;
}

FSTRACE_DECL(ASYNC_QUEUESTREAM_READ, "UID=%64u WANT=%z GOT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_QUEUESTREAM_READ_DUMP, "UID=%64u DATA=%B");

ssize_t queuestream_read(queuestream_t *qstr, void *buf, size_t count)
{
    ssize_t n = do_read(qstr, buf, count);
    FSTRACE(ASYNC_QUEUESTREAM_READ, qstr->uid, count, n);
    FSTRACE(ASYNC_QUEUESTREAM_READ_DUMP, qstr->uid, buf, n);
    return n;
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return queuestream_read(obj, buf, count);
}

FSTRACE_DECL(ASYNC_QUEUESTREAM_CLOSE, "UID=%64u RELEASED=%b");

void queuestream_close(queuestream_t *qstr)
{
    FSTRACE(ASYNC_QUEUESTREAM_CLOSE, qstr->uid, qstr->released);
    assert(!qstr->closed);
    while (!list_empty(qstr->queue)) {
        list_elem_t *head_elem = list_get_first(qstr->queue);
        bytestream_1 *head = (bytestream_1 *) list_elem_get_value(head_elem);
        bytestream_1_close(*head);
        fsfree(head);
        list_remove(qstr->queue, head_elem);
    }
    destroy_list(qstr->queue);
    if (qstr->released)
        async_wound(qstr->async, qstr);
    qstr->closed = true;
}

static void _close(void *obj)
{
    queuestream_close(obj);
}

FSTRACE_DECL(ASYNC_QUEUESTREAM_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void queuestream_register_callback(queuestream_t *qstr, action_1 action)
{
    FSTRACE(ASYNC_QUEUESTREAM_REGISTER, qstr->uid, action.obj, action.act);
    qstr->notifier = action;
}

static void _register_callback(void *obj, action_1 action)
{
    queuestream_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_QUEUESTREAM_UNREGISTER, "UID=%64u");

void queuestream_unregister_callback(queuestream_t *qstr)
{
    FSTRACE(ASYNC_QUEUESTREAM_UNREGISTER, qstr->uid);
    qstr->notifier = NULL_ACTION_1;
}

static void _unregister_callback(void *obj)
{
    queuestream_unregister_callback(obj);
}

static const struct bytestream_1_vt queuestream_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback
};

bytestream_1 queuestream_as_bytestream_1(queuestream_t *qstr)
{
    return (bytestream_1) { qstr, &queuestream_vt };
}
