#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <assert.h>
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

#define VERIFY(tc) verify(#tc, tc)

static int enable_all(void *data, const char *id)
{
    return 1;
}

int main(int argc, const char *const *argv)
{
    if (argc == 2) {
        if (!strcmp(argv[1], "--trace")) {
            fstrace_t *trace = fstrace_direct(stderr);
            fstrace_declare_globals(trace);
            fstrace_select(trace, enable_all, NULL);

        } else {
            fprintf(stderr, "Usage: asynctest [ --trace ]\n");
            return EXIT_FAILURE;
        }
    }
    reallocator = fs_get_reallocator();
    fs_set_reallocator(test_realloc);
    VERIFY(test_async_timer_start);
    VERIFY(test_async_timer_cancel);
    VERIFY(test_async_register);
    VERIFY(test_async_loop_protected);
    VERIFY(test_async_poll);
    VERIFY(test_async_old_school);
    VERIFY(test_zerostream);
    VERIFY(test_nicestream);
    VERIFY(test_emptystream);
    VERIFY(test_drystream);
    VERIFY(test_blockingstream);
    VERIFY(test_stringstream);
    VERIFY(test_blobstream);
    VERIFY(test_chunkdecoder);
    VERIFY(test_chunkencoder);
    VERIFY(test_queuestream);
    VERIFY(test_relaxed_queuestream);
    VERIFY(test_chunkframer);
    VERIFY(test_naiveframer);
    VERIFY(test_json);
    VERIFY(test_multipart);
    VERIFY(test_concatstream);
    VERIFY(test_tcp_connection);
    VERIFY(test_pacerstream);
    VERIFY(test_clobberstream);
    VERIFY(test_pausestream);
    VERIFY(test_probestream);
    VERIFY(test_base64encoder);
    VERIFY(test_iconvstream);
    return failures;
}
