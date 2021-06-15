#include "asynctest-emptystream.h"

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include <async/emptystream.h>

VERDICT test_emptystream(void)
{
    uint8_t buffer[100];
    ssize_t count = bytestream_1_read(emptystream, buffer, sizeof buffer);
    if (count != 0) {
        tlog("Unexpected count: %d (errno = %d)", (int) count, (int) errno);
        return FAIL;
    }
    bytestream_1_close(emptystream);
    count = bytestream_1_read(emptystream, buffer, sizeof buffer);
    if (count != 0) {
        tlog("Unexpected count: %d (errno = %d)", (int) count, (int) errno);
        return FAIL;
    }
    return posttest_check(PASS);
}
