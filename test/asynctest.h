#ifndef __ASYNCTEST__
#define __ASYNCTEST__

typedef enum {
    FAIL = 0,
    PASS = 1
} VERDICT;

void tlog(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
int posttest_check(int tentative_verdict);

#endif
