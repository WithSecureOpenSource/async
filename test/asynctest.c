#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <regex.h>
#include <sys/time.h>
#include <fsdyn/fsalloc.h>
#include <fstrace.h>
#include "asynctest.h"
#include "asynctest-timer.h"
#include "asynctest-poll.h"
#include "asynctest-old-school.h"
#include "asynctest-zerostream.h"
#include "asynctest-nicestream.h"
#include "asynctest-emptystream.h"
#include "asynctest-drystream.h"
#include "asynctest-blockingstream.h"
#include "asynctest-stringstream.h"
#include "asynctest-blobstream.h"
#include "asynctest-chunkdecoder.h"
#include "asynctest-chunkencoder.h"
#include "asynctest-queuestream.h"
#include "asynctest-framers.h"
#include "asynctest-json.h"
#include "asynctest-multipart.h"
#include "asynctest-subprocess.h"
#include "asynctest-concatstream.h"
#include "asynctest-tcp.h"
#include "asynctest-pacerstream.h"
#include "asynctest-probestream.h"
#include "asynctest-clobberstream.h"
#include "asynctest-pausestream.h"
#include "asynctest-base64encoder.h"
#include "asynctest-iconvstream.h"

static int failures = 0;

static void timestamp(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t t = tv.tv_sec;
    struct tm tm;
    gmtime_r(&t, &tm);
    char s[50];
    strftime(s, sizeof s, "%F %T", &tm);
    fprintf(stderr, "%s.%03d: ", s, (int) (tv.tv_usec / 1000));
}

void tlog(const char *format, ...)
{
    timestamp();
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int outstanding_object_count = 0;
static int log_allocation = 0;  /* set in debugger */

static fs_realloc_t reallocator;

static void *test_realloc(void *ptr, size_t size)
{
    void *obj = (*reallocator)(ptr, size);
    assert(obj != NULL || size == 0);
    if (ptr != NULL) {
        outstanding_object_count--;
        if (log_allocation)
            tlog("free %p", ptr);
    }
    if (obj != NULL) {
        if (!ptr)
            memset(obj, 0xa5, size);
        outstanding_object_count++;
        if (log_allocation)
            tlog("alloc %p", obj);
    }
    return obj;
}

int posttest_check(int tentative_verdict)
{
    if (tentative_verdict != PASS)
        return tentative_verdict;
    if (outstanding_object_count != 0) {
        tlog("Garbage generated (outstanding_object_count = %d)",
             outstanding_object_count);
        return FAIL;
    }
    return PASS;
}

static void verify(const char *name, VERDICT (*testcase)(void))
{
    outstanding_object_count = 0;
    tlog("Begin %s", name);
    switch (testcase()) {
        case PASS:
            tlog("PASS");
            break;
        case FAIL:
            tlog("FAIL");
            failures++;
            break;
        default:
            assert(0);
    }
    tlog("End %s", name);
}

static int enable_all(void *data, const char *id)
{
    return 1;
}

static void bad_usage()
{
    fprintf(stderr, "Usage: asynctest [ --test-include PATTERN ] [ --trace ]\n");
    exit(EXIT_FAILURE);
}

typedef struct {
    const char *name;
    VERDICT (*testcase)(void);
} testcase_t;

#define TESTCASE(tc) { #tc, tc }

static const testcase_t testcases[] = {
    TESTCASE(test_async_timer_start),
    TESTCASE(test_async_timer_cancel),
    TESTCASE(test_async_register),
    TESTCASE(test_async_loop_protected),
    TESTCASE(test_async_poll),
    TESTCASE(test_async_old_school),
    TESTCASE(test_zerostream),
    TESTCASE(test_nicestream),
    TESTCASE(test_emptystream),
    TESTCASE(test_drystream),
    TESTCASE(test_blockingstream),
    TESTCASE(test_stringstream),
    TESTCASE(test_blobstream),
    TESTCASE(test_chunkdecoder),
    TESTCASE(test_chunkencoder),
    TESTCASE(test_queuestream),
    TESTCASE(test_relaxed_queuestream),
    TESTCASE(test_chunkframer),
    TESTCASE(test_naiveframer),
    TESTCASE(test_jsonyield),
    TESTCASE(test_jsondecoder),
    TESTCASE(test_multipart),
    TESTCASE(test_concatstream),
    TESTCASE(test_tcp_connection),
    TESTCASE(test_pacerstream),
    TESTCASE(test_clobberstream),
    TESTCASE(test_pausestream),
    TESTCASE(test_probestream),
    TESTCASE(test_base64encoder),
    TESTCASE(test_iconvstream),
    TESTCASE(test_subprocess),
};

int main(int argc, const char *const *argv)
{
    fstrace_t *trace = fstrace_direct(stderr);
    fstrace_declare_globals(trace);

    const char *include = ".";
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "--test-include")) {
            if (++i >= argc)
                bad_usage();
            include = argv[i++];
            continue;
        }
        if (!strcmp(argv[i], "--trace")) {
            fstrace_select(trace, enable_all, NULL);
            i++;
            continue;
        }
        bad_usage();
    }

    regex_t include_re;
    int status = regcomp(&include_re, include, REG_EXTENDED | REG_NOSUB);
    if (status)
        bad_usage();
    reallocator = fs_get_reallocator();
    fs_set_reallocator(test_realloc);
    for (i = 0; i < sizeof(testcases) / sizeof(testcases[0]); i++)
        if (!regexec(&include_re, testcases[i].name, 0, NULL, 0))
            verify(testcases[i].name, testcases[i].testcase);
    fstrace_close(trace);
    regfree(&include_re);
    return failures;
}
