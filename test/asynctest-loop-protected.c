#include "asynctest-loop-protected.h"

#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include <async/async.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    tester_base_t base;
    int counter;
} tester_t;

enum { MT_COUNT = 5 };

static void mt_timeout(tester_t *tester)
{
    if (++tester->counter == MT_COUNT)
        tester->base.verdict = PASS;
}

static void *aux_thread(void *arg)
{
    tester_t *tester = arg;
    int i;
    for (i = 0; i < MT_COUNT; i++) {
        pthread_mutex_lock(&mutex);
        async_timer_start(tester->base.async,
                          async_now(tester->base.async) + ASYNC_S,
                          (action_1) { tester, (act_1) mt_timeout });
        pthread_mutex_unlock(&mutex);
        sleep(1);
    }
    return NULL;
}

static void lock(void *ptr)
{
    pthread_mutex_t *mutex = ptr;
    pthread_mutex_lock(mutex);
}

static void unlock(void *ptr)
{
    pthread_mutex_t *mutex = ptr;
    pthread_mutex_unlock(mutex);
}

VERDICT test_async_loop_protected(void)
{
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    async_t *async = make_async();
    tester_t tester = {};
    init_test(&tester.base, async, MT_COUNT + 2);
    pthread_create(&thread, &attr, aux_thread, &tester);
    pthread_mutex_lock(&mutex);
    if (async_loop_protected(async, lock, unlock, &mutex) < 0)
        tlog("Unexpected error from async_loop_protected: %d", errno);
    pthread_mutex_unlock(&mutex);
    destroy_async(async);
    pthread_join(thread, NULL);
    return posttest_check(tester.base.verdict);
}
