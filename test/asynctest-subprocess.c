#include "asynctest-subprocess.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <async/subprocess.h>
#include <fsdyn/bytearray.h>
#include <fsdyn/fsalloc.h>

typedef struct {
    tester_base_t base;
    bytestream_1 stdout;
    byte_array_t *buffer;
    char *output;
} tester_t;

static ssize_t read_data(void *obj, void *buf, size_t count)
{
    bytestream_1 *stream = obj;
    return bytestream_1_read(*stream, buf, count);
}

static void verify_read(tester_t *tester)
{
    if (!tester->base.async)
        return;
    ssize_t count = byte_array_append_stream(tester->buffer, read_data,
                                             &tester->stdout, 1024);
    if (count < 0) {
        if (errno != EAGAIN) {
            tlog("Errno %d from bytestream_1_read", errno);
            quit_test(&tester->base);
        }
        return;
    }
    if (!count) {
        if (!strcmp(byte_array_data(tester->buffer), tester->output))
            tester->base.verdict = PASS;
        quit_test(&tester->base);
        return;
    }
    action_1 verification_cb = { tester, (act_1) verify_read };
    async_execute(tester->base.async, verification_cb);
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
        .buffer = make_byte_array(1024),
        .output = "test",
    };
    init_test(&tester.base, async, 10);
    action_1 post_fork_cb = { &tester, (act_1) execute };
    subprocess_t *subprocess =
        open_subprocess(async, make_list(), true, false, post_fork_cb);
    tester.stdout = subprocess_release_stdout(subprocess);
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
    return posttest_check(tester.base.verdict);
}
