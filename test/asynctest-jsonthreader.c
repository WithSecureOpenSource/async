#include "asynctest-jsonthreader.h"

#include <errno.h>

#include <async/jsonthreader.h>
#include <fsdyn/integer.h>

typedef struct {
    tester_base_t base;
    jsonthreader_t *threader;
    json_thing_t *message;
} tester_t;

static void probe_threader(tester_t *tester)
{
    if (!tester->base.async)
        return;
    json_thing_t *response = jsonthreader_receive(tester->threader);
    if (!response) {
        if (errno != EAGAIN) {
            tlog("Errno %d from jsonthreader_receive", errno);
            quit_test(&tester->base);
        }
        return;
    }
    if (json_thing_equal(response, tester->message, 0.1))
        tester->base.verdict = PASS;
    json_destroy_thing(response);
    quit_test(&tester->base);
}

static json_thing_t *handle_request(void *obj, json_thing_t *request)
{
    return json_clone(request);
}

static VERDICT test(unsigned max_parallel)
{
    async_t *async = make_async();
    tester_t tester = {
        .message = json_make_string("test"),
    };
    init_test(&tester.base, async, 10);
    action_1 post_fork_cb = { NULL, (act_1) reinit_trace };
    list_t *fds_to_keep = make_list();
    list_append(fds_to_keep, as_integer(0));
    list_append(fds_to_keep, as_integer(1));
    list_append(fds_to_keep, as_integer(2));
    tester.threader =
        make_jsonthreader(async, fds_to_keep, post_fork_cb, handle_request,
                          NULL, 8192, max_parallel);
    action_1 probe_cb = { &tester, (act_1) probe_threader };
    jsonthreader_register_callback(tester.threader, probe_cb);
    async_execute(async, probe_cb);
    jsonthreader_send(tester.threader, tester.message);
    if (async_loop(async) < 0)
        tlog("Unexpected error from async_loop: %d", errno);
    json_destroy_thing(tester.message);
    jsonthreader_terminate(tester.threader);
    destroy_jsonthreader(tester.threader);
    destroy_async(async);
    return posttest_check(tester.base.verdict);
}

VERDICT test_jsonthreader(void)
{
    return test(1);
}

VERDICT test_jsonthreader_mt(void)
{
    return test(2);
}
