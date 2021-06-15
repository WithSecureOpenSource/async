#include "notification.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>
#include <unixkit/unixkit.h>

#include "async_version.h"

struct notification {
    async_t *async;
    uint64_t uid;
    action_1 action;
    int readfd, writefd;
};

FSTRACE_DECL(ASYNC_NOTIFICATION_SPURIOUS_PROBE, "UID=%64u");
FSTRACE_DECL(ASYNC_NOTIFICATION_NOTIFY, "UID=%64u");

static void probe(notification_t *notification)
{
    char buf[200];
    ssize_t count = read(notification->readfd, buf, sizeof buf);
    if (count < 0) {
        assert(errno == EAGAIN);
        FSTRACE(ASYNC_NOTIFICATION_SPURIOUS_PROBE, notification->uid);
        return;
    }
    for (;;) {
        assert(count > 0);
        count = read(notification->readfd, buf, sizeof buf);
        if (count < 0) {
            assert(errno == EAGAIN);
            FSTRACE(ASYNC_NOTIFICATION_NOTIFY, notification->uid);
            action_1_perf(notification->action);
            return;
        }
    }
}

FSTRACE_DECL(ASYNC_NOTIFICATION_CREATE_FAIL, "ASYNC=%p ERRNO=%e");
FSTRACE_DECL(ASYNC_NOTIFICATION_CREATE,
             "UID=%64u PTR=%p ASYNC=%p OBJ=%p ACT=%p");

notification_t *make_notification(async_t *async, action_1 action)
{
    int fd[2];
    if (!unixkit_pipe(fd)) {
        FSTRACE(ASYNC_NOTIFICATION_CREATE_FAIL, async);
        return NULL;
    }
    fcntl(fd[1], F_SETFL, fcntl(fd[1], F_GETFL, 0) | O_NONBLOCK);
    notification_t *notification = fsalloc(sizeof *notification);
    notification->async = async;
    notification->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_NOTIFICATION_CREATE, notification->uid, notification, async,
            action.obj, action.act);
    notification->action = action;
    notification->readfd = fd[0];
    notification->writefd = fd[1];
    action_1 probe_cb = { notification, (act_1) probe };
    async_register(async, fd[0], probe_cb);
    async_execute(async, probe_cb);
    return notification;
}

FSTRACE_DECL(ASYNC_NOTIFICATION_DESTROY, "UID=%64u");

void destroy_notification(notification_t *notification)
{
    FSTRACE(ASYNC_NOTIFICATION_DESTROY, notification->uid);
    async_unregister(notification->async, notification->readfd);
    close(notification->readfd);
    close(notification->writefd);
    async_wound(notification->async, notification);
    notification->async = NULL;
}

FSTRACE_DECL(ASYNC_NOTIFICATION_ISSUE, "UID=%64u");

void issue_notification(notification_t *notification)
{
    FSTRACE(ASYNC_NOTIFICATION_ISSUE, notification->uid);
    if (write(notification->writefd, notification, 1) < 0)
        assert(errno == EAGAIN);
}
