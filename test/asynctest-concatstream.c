#include "asynctest-concatstream.h"

#include <errno.h>
#include <string.h>

#include <async/concatstream.h>
#include <async/stringstream.h>

#define CONC_S1  "Stop "
#define CONC_S2  "right "
#define CONC_S3  "there!"
#define CONC_ALL (CONC_S1 CONC_S2 CONC_S3)

VERDICT test_concatstream(void)
{
    async_t *async = make_async();
    stringstream_t *s1 = open_stringstream(async, CONC_S1);
    stringstream_t *s2 = open_stringstream(async, CONC_S2);
    stringstream_t *s3 = open_stringstream(async, CONC_S3);
    bytestream_1 streams[] = {
        stringstream_as_bytestream_1(s1),
        stringstream_as_bytestream_1(s2),
        stringstream_as_bytestream_1(s3),
    };
    concatstream_t *conc =
        concatenate_streams(async, streams, sizeof streams / sizeof streams[0]);

    char buffer[100];
    size_t offset = 0;
    for (;;) {
        ssize_t count =
            concatstream_read(conc, buffer + offset, sizeof buffer - offset);
        if (count < 0) {
            tlog("Unexpected error %d (errno %d) from concatstream",
                 (int) count, (int) errno);
            return FAIL;
        }
        if (count == 0)
            break;
        offset += count;
        if (offset > strlen(CONC_ALL)) {
            tlog("Too many bytes from concatstream");
            return FAIL;
        }
    }
    if (offset < strlen(CONC_ALL)) {
        tlog("Too few bytes from concatstream");
        return FAIL;
    }
    if (strncmp(buffer, CONC_ALL, offset) != 0) {
        tlog("Bad bytes from concatstream");
        return FAIL;
    }
    concatstream_close(conc);
    destroy_async(async);
    return posttest_check(PASS);
}
