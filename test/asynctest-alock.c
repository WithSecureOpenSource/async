#include "asynctest-alock.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <async/alock.h>
#include <fsdyn/fsalloc.h>

typedef struct {
    tester_base_t base;
    const char *lock_path;
    int lock_fd;
    alock_t *alock;
    action_1 probe_cb;
    enum { LOCKING_NOFILE, LOCKING, UNLOCKING } state;
} tester_t;

static void verify_lock_nofile(tester_t *tester)
{
    bool locked;
    if (alock_check(tester->alock, &locked)) {
        tlog("Expected ENOENT, got success");
        quit_test(&tester->base);
        return;
    }
    if (errno == EAGAIN)
        return;
    if (errno != ENOENT) {
        tlog("Expected ENOENT, got %d", errno);
        quit_test(&tester->base);
        return;
    }
    tester->lock_fd =
        open(tester->lock_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (tester->lock_fd < 0) {
        tlog("Failed to open lock file (errno %d)", errno);
        quit_test(&tester->base);
        return;
    }
    tester->state = LOCKING;
    alock_lock(tester->alock);
    async_execute(tester->base.async, tester->probe_cb);
}

static void verify_lock(tester_t *tester)
{
    bool locked;
    if (!alock_check(tester->alock, &locked)) {
        if (errno != EAGAIN) {
            tlog("Unexpected error %d", errno);
            quit_test(&tester->base);
        }
        return;
    }
    if (!locked) {
        tlog("Bad lock state after lock");
        quit_test(&tester->base);
        return;
    }
    if (!flock(tester->lock_fd, LOCK_EX | LOCK_NB)) {
        tlog("Expected EWOULDBLOCK from flock, got success");
        quit_test(&tester->base);
        return;
    }
    if (errno != EWOULDBLOCK) {
        tlog("Expected EWOULDBLOCK from flock, got %d", errno);
        quit_test(&tester->base);
        return;
    }
    tester->state = UNLOCKING;
    alock_unlock(tester->alock);
    async_execute(tester->base.async, tester->probe_cb);
}

static void verify_unlock(tester_t *tester)
{
    bool locked;
    if (!alock_check(tester->alock, &locked)) {
        if (errno != EAGAIN) {
            tlog("Unexpected error %d", errno);
            quit_test(&tester->base);
        }
        return;
    }
    if (locked) {
        tlog("Bad lock state after unlock");
        quit_test(&tester->base);
        return;
    }
    if (flock(tester->lock_fd, LOCK_EX | LOCK_NB) < 0) {
        tlog("Unexpected error %d from flock", errno);
        quit_test(&tester->base);
        return;
    }
    tester->base.verdict = PASS;
    quit_test(&tester->base);
}

static void probe_lock(tester_t *tester)
{
    if (!tester->base.async)
        return;
    switch (tester->state) {
        case LOCKING_NOFILE:
            return verify_lock_nofile(tester);
        case LOCKING:
            return verify_lock(tester);
        case UNLOCKING:
            return verify_unlock(tester);
        default:
            assert(false);
    }
}

VERDICT test_alock(void)
{
    const char *lock_path = "/tmp/asynctest.lock";
    unlink(lock_path);
    async_t *async = make_async();
    tester_t tester = {
        .lock_path = lock_path,
        .alock = make_alock(async, lock_path, NULL_ACTION_1),
        .probe_cb = { &tester, (act_1) probe_lock },
        .state = LOCKING_NOFILE,
    };
    init_test(&tester.base, async, 10);
    alock_register_callback(tester.alock, tester.probe_cb);
    alock_lock(tester.alock);
    async_execute(async, tester.probe_cb);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    destroy_alock(tester.alock);
    destroy_async(async);
    return posttest_check(tester.base.verdict);
}
