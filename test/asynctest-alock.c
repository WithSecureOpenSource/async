#include "asynctest-alock.h"

#include <async/alock.h>
#include <fsdyn/fsalloc.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

typedef struct {
    async_t *async;
    const char *lock_path;
    int lock_fd;
    alock_t *alock;
    action_1 probe_cb;
    enum { LOCKING_NOFILE, LOCKING, UNLOCKING } state;
    async_timer_t *timer;
    VERDICT verdict;
} tester_t;

static void do_quit(tester_t *tester)
{
    action_1 quitter = { tester->async, (act_1) async_quit_loop };
    tester->timer = NULL;
    async_execute(tester->async, quitter);
    tester->async = NULL;
}

static void quit_test(tester_t *tester)
{
    async_timer_cancel(tester->async, tester->timer);
    do_quit(tester);
}

static void test_timeout(tester_t *tester)
{
    tlog("Test timeout");
    do_quit(tester);
}

static void verify_lock_nofile(tester_t *tester)
{
    bool locked;
    if (alock_check(tester->alock, &locked)) {
        tlog("Expected ENOENT, got success");
        quit_test(tester);
        return;
    }
    if (errno == EAGAIN)
        return;
    if (errno != ENOENT) {
        tlog("Expected ENOENT, got %d", errno);
        quit_test(tester);
        return;
    }
    tester->lock_fd = open(tester->lock_path, O_CREAT | O_TRUNC, 0644);
    if (tester->lock_fd < 0) {
        tlog("Failed to open lock file (errno %d)", errno);
        quit_test(tester);
        return;
    }
    tester->state = LOCKING;
    alock_lock(tester->alock);
    async_execute(tester->async, tester->probe_cb);
}

static void verify_lock(tester_t *tester)
{
    bool locked;
    if (!alock_check(tester->alock, &locked)) {
        if (errno != EAGAIN) {
            tlog("Unexpected error %d", errno);
            quit_test(tester);
        }
        return;
    }
    if (!locked) {
        tlog("Bad lock state after lock");
        quit_test(tester);
        return;
    }
    if (!flock(tester->lock_fd, LOCK_EX | LOCK_NB)) {
        tlog("Expected EWOULDBLOCK from flock, got success");
        quit_test(tester);
        return;
    }
    if (errno != EWOULDBLOCK) {
        tlog("Expected EWOULDBLOCK from flock, got %d", errno);
        quit_test(tester);
        return;
    }
    tester->state = UNLOCKING;
    alock_unlock(tester->alock);
    async_execute(tester->async, tester->probe_cb);
}

static void verify_unlock(tester_t *tester)
{
    bool locked;
    if (!alock_check(tester->alock, &locked)) {
        if (errno != EAGAIN) {
            tlog("Unexpected error %d", errno);
            quit_test(tester);
        }
        return;
    }
    if (locked) {
        tlog("Bad lock state after unlock");
        quit_test(tester);
        return;
    }
    if (flock(tester->lock_fd, LOCK_EX | LOCK_NB) < 0) {
        tlog("Unexpected error %d from flock", errno);
        quit_test(tester);
        return;
    }
    tester->verdict = PASS;
    quit_test(tester);
}

static void probe_lock(tester_t *tester)
{
    if (!tester->async)
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
        .async = async,
        .lock_path = lock_path,
        .alock = make_alock(async, lock_path, NULL_ACTION_1),
        .probe_cb = { &tester, (act_1) probe_lock },
        .state = LOCKING_NOFILE,
        .verdict = FAIL,
    };
    action_1 timeout_cb = { &tester, (act_1) test_timeout };
    enum { MAX_DURATION = 10 };
    tlog("  max duration = %d s", MAX_DURATION);
    tester.timer = async_timer_start(async,
                                     async_now(async) + MAX_DURATION * ASYNC_S,
                                     timeout_cb);
    alock_register_callback(tester.alock, tester.probe_cb);
    alock_lock(tester.alock);
    async_execute(async, tester.probe_cb);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    destroy_alock(tester.alock);
    destroy_async(async);
    return posttest_check(tester.verdict);
}
