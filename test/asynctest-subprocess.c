#include "asynctest-subprocess.h"

#include <async/subprocess.h>
#include <fsdyn/bytearray.h>
#include <fsdyn/fsalloc.h>

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    async_t *async;
    bytestream_1 stdout;
    byte_array_t *buffer;
    char *output;
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

static ssize_t read_data(void *obj, void *buf, size_t count)
{
    bytestream_1 *stream = obj;
    return bytestream_1_read(*stream, buf, count);
}

static void verify_read(tester_t *tester)
{
    if (!tester->async)
        return;
    ssize_t count = byte_array_append_stream(tester->buffer,
                                             read_data,
                                             &tester->stdout,
                                             1024);
    if (count < 0) {
        if (errno != EAGAIN) {
            tlog("Errno %d from bytestream_1_read", errno);
            quit_test(tester);
        }
        return;
    }
    if (!count) {
        if (!strcmp(byte_array_data(tester->buffer), tester->output))
            tester->verdict = PASS;
        quit_test(tester);
        return;
    }
    action_1 verification_cb = { tester, (act_1) verify_read };
    async_execute(tester->async, verification_cb);
}

static void execute(tester_t *tester)
{
    char *args[] = {
        "/usr/bin/printf",
        tester->output,
        NULL,
    };
    execvp(args[0], args);
}

VERDICT test_subprocess(void)
{
    async_t *async = make_async();
    tester_t tester = {
        .async = async,
        .buffer = make_byte_array(1024),
        .output = "test",
        .verdict = FAIL,
    };
    action_1 post_fork_cb = { &tester, (act_1) execute };
    subprocess_t *subprocess =
        open_subprocess(async, make_list(), true, false, post_fork_cb);
    tester.stdout = subprocess_release_stdout(subprocess);
    action_1 timeout_cb = { &tester, (act_1) test_timeout };
    enum { MAX_DURATION = 10 };
    tlog("  max duration = %d s", MAX_DURATION);
    tester.timer = async_timer_start(async,
                                     async_now(async) + MAX_DURATION * ASYNC_S,
                                     timeout_cb);
    action_1 verification_cb = { &tester, (act_1) verify_read };
    bytestream_1_register_callback(tester.stdout, verification_cb);
    async_execute(async, verification_cb);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    int exit_status;
    assert(subprocess_wait(subprocess, &exit_status));
    assert(exit_status == 0);
    subprocess_close(subprocess);
    bytestream_1_close(tester.stdout);
    destroy_byte_array(tester.buffer);
    destroy_async(async);
    return posttest_check(tester.verdict);
}
