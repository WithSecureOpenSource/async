#include "asynctest.h"

#include <assert.h>
#include <errno.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <fsdyn/fsalloc.h>
#include <fstrace.h>

#include "asynctest-alock.h"
#include "asynctest-base64encoder.h"
#include "asynctest-blobstream.h"
#include "asynctest-blockingstream.h"
#include "asynctest-chunkdecoder.h"
#include "asynctest-chunkencoder.h"
#include "asynctest-clobberstream.h"
#include "asynctest-concatstream.h"
#include "asynctest-drystream.h"
#include "asynctest-emptystream.h"
#include "asynctest-framers.h"
#include "asynctest-fsadns.h"
#include "asynctest-iconvstream.h"
#include "asynctest-json.h"
#include "asynctest-jsonserver.h"
#include "asynctest-jsonthreader.h"
#include "asynctest-loop-protected.h"
#include "asynctest-multipart.h"
#include "asynctest-nicestream.h"
#include "asynctest-old-school.h"
#include "asynctest-pacerstream.h"
#include "asynctest-pausestream.h"
#include "asynctest-poll.h"
#include "asynctest-probestream.h"
#include "asynctest-queuestream.h"
#include "asynctest-stringstream.h"
#include "asynctest-subprocess.h"
#include "asynctest-tcp.h"
#include "asynctest-timer.h"
#include "asynctest-zerostream.h"

static void do_quit(tester_base_t *tester)
{
    action_1 quit_cb = { tester->async, (act_1) async_quit_loop };
    tester->timer = NULL;
    async_execute(tester->async, quit_cb);
    tester->async = NULL;
}

static void test_timeout(tester_base_t *tester)
{
    tlog("Test timeout");
    do_quit(tester);
}

void init_test(tester_base_t *tester, async_t *async, int max_duration)
{
    tlog("  max duration = %d s", max_duration);
    tester->async = async;
    tester->verdict = FAIL;
    action_1 timeout_cb = { tester, (act_1) test_timeout };
    tester->timer =
        async_timer_start(async, async_now(async) + max_duration * ASYNC_S,
                          timeout_cb);
}

void quit_test(tester_base_t *tester)
{
    async_timer_cancel(tester->async, tester->timer);
    do_quit(tester);
}

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

void tlog_string(const char *str)
{
    timestamp();
    fputs(str, stderr);
    fputc('\n', stderr);
}

static int outstanding_object_count = 0;
static int log_allocation = 0; /* set in debugger */

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

static void test_reallocator_counter(int count)
{
    outstanding_object_count += count;
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

static void bad_usage()
{
    fprintf(stderr,
            "Usage: asynctest [ <options> ]\n"
            "\n"
            "Options:\n"
            "    --test-include <regex>\n"
            "    --trace-include <regex>\n"
            "    --trace-exclude <regex>\n");
    exit(EXIT_FAILURE);
}

typedef struct {
    const char *name;
    VERDICT (*testcase)(void);
} testcase_t;

#define TESTCASE(tc) \
    {                \
#tc, tc      \
    }

static const testcase_t testcases[] = {
    TESTCASE(test_async_timer_start),
    TESTCASE(test_async_timer_cancel),
    TESTCASE(test_async_register),
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
    TESTCASE(test_jsonserver),
    TESTCASE(test_jsonthreader),
    TESTCASE(test_jsonthreader_mt),
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
    TESTCASE(test_alock),
    TESTCASE(test_fsadns),
};

static const testcase_t mt_testcases[] = {
    TESTCASE(test_async_loop_protected),
};

static fstrace_t *trace;

void reinit_trace(void)
{
    fstrace_reopen(trace);
}

int main(int argc, const char *const *argv)
{
    trace = fstrace_direct(stderr);
    fstrace_declare_globals(trace);

    const char *include = ".";
    const char *trace_include = NULL;
    const char *trace_exclude = NULL;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "--test-include")) {
            if (++i >= argc)
                bad_usage();
            include = argv[i++];
            continue;
        }
        if (!strcmp(argv[i], "--trace-include")) {
            if (++i >= argc)
                bad_usage();
            trace_include = argv[i++];
            continue;
        }
        if (!strcmp(argv[i], "--trace-exclude")) {
            if (++i >= argc)
                bad_usage();
            trace_exclude = argv[i++];
            continue;
        }
        bad_usage();
    }

    if (!fstrace_select_regex(trace, trace_include, trace_exclude))
        bad_usage();

    regex_t include_re;
    int status = regcomp(&include_re, include, REG_EXTENDED | REG_NOSUB);
    if (status)
        bad_usage();
    reallocator = fs_get_reallocator();
    fs_set_reallocator(test_realloc);
    fs_set_reallocator_counter(test_reallocator_counter);
    for (i = 0; i < sizeof(testcases) / sizeof(testcases[0]); i++)
        if (!regexec(&include_re, testcases[i].name, 0, NULL, 0))
            verify(testcases[i].name, testcases[i].testcase);
    for (i = 0; i < sizeof(mt_testcases) / sizeof(mt_testcases[0]); i++)
        if (!regexec(&include_re, mt_testcases[i].name, 0, NULL, 0))
            verify(mt_testcases[i].name, mt_testcases[i].testcase);
    fstrace_close(trace);
    regfree(&include_re);
    return failures;
}
