#ifndef __ASYNCTEST__
#define __ASYNCTEST__

#include <async/async.h>

typedef enum {
    FAIL = 0,
    PASS = 1,
} VERDICT;

typedef struct {
    async_t *async;
    VERDICT verdict;
    async_timer_t *timer;
} tester_base_t;

void init_test(tester_base_t *tester, async_t *async, int max_duration);
void quit_test(tester_base_t *tester);

void reinit_trace(void);

void tlog(const char *format, ...) __attribute__((format(printf, 1, 2)));
void tlog_string(const char *str);
int posttest_check(int tentative_verdict);

#endif
