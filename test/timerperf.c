#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <async/async.h>

typedef struct {
    async_t *async;
} global_t;

typedef struct {
    global_t *g;
    async_timer_t *timer;
    bool last;
} context_t;

enum {
    N = 10000000,
};

static context_t *new_context(global_t *g, bool last)
{
    context_t *context = fsalloc(sizeof *context);
    context->g = g;
    context->last = last;
    return context;
}

static void finish(context_t *context)
{
    async_t *async = context->g->async;
    async_timer_cancel(async, context->timer);
    if (context->last)
        async_quit_loop(async);
    fsfree(context);
}

static void start(context_t *context)
{
    async_t *async = context->g->async;
    action_1 dummy_cb = { 0 };
    context->timer =
        async_timer_start(async, async_now(async) + ASYNC_H, dummy_cb);
    async_execute(async, (action_1) { context, (act_1) finish });
}

static void kick_off(global_t *g)
{
    for (int i = 1; i < N; i++) {
        action_1 start_cb = { new_context(g, false), (act_1) start };
        async_execute(g->async, start_cb);
    }
    action_1 start_cb = { new_context(g, true), (act_1) start };
    async_execute(g->async, start_cb);
}

int main()
{
    async_t *async = make_async();
    global_t g = {
        .async = async,
    };
    uint64_t t0 = async_now(async);
    kick_off(&g);
    while (async_loop(async) < 0)
        if (errno != EINTR) {
            perror("fsadns_test");
            return EXIT_FAILURE;
        }
    async_flush(async, async_now(async) + ASYNC_MIN);
    uint64_t t1 = async_now(async);
    destroy_async(async);
    printf("%g\n", (double) (t1 - t0) / ASYNC_S);
    return EXIT_SUCCESS;
}
