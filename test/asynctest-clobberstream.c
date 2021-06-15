#include "asynctest-clobberstream.h"

#include <errno.h>
#include <string.h>

#include <async/async.h>
#include <async/clobberstream.h>
#include <async/substream.h>
#include <async/zerostream.h>

enum {
    OFFSET = 10,
    MASK = -1,
    TOTAL_BYTE_COUNT = 50,
};

VERDICT test_clobberstream(void)
{
    async_t *async = make_async();
    substream_t *substr =
        make_substream(async, zerostream, SUBSTREAM_CLOSE_AT_END, 0,
                       TOTAL_BYTE_COUNT);
    clobberstream_t *clstr =
        clobber(async, substream_as_bytestream_1(substr), OFFSET, MASK);

    uint8_t buffer[TOTAL_BYTE_COUNT];
    ssize_t count = clobberstream_read(clstr, buffer, OFFSET + 2);
    if (count != OFFSET + 2) {
        tlog("Unexpected error %d (errno %d)", (int) count, (int) errno);
        return FAIL;
    }
    count = clobberstream_read(clstr, buffer + OFFSET + 2,
                               sizeof buffer - (OFFSET + 2));
    if (count != TOTAL_BYTE_COUNT - (OFFSET + 2)) {
        tlog("Unexpected error %d (errno %d)", (int) count, (int) errno);
        return FAIL;
    }
    uint64_t value;
    memcpy(&value, buffer + OFFSET, sizeof value);
    if (value != MASK) {
        tlog("Unexpected clobber value");
        return FAIL;
    }
    clobberstream_close(clstr);
    destroy_async(async);
    return posttest_check(PASS);
}
