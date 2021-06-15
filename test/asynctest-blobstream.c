#include "asynctest-blobstream.h"

#include <errno.h>
#include <string.h>

#include <async/async.h>
#include <async/blobstream.h>

VERDICT test_blobstream(void)
{
    async_t *async = make_async();
    blobstream_t *blobstr = open_blobstream(async, "Hello world", 11);
    char buffer[100];
    ssize_t count;
    count = blobstream_read(blobstr, buffer, 5);
    if (count != 5) {
        tlog("Unexpected error %d (errno %d) from blobstream_read", (int) count,
             (int) errno);
        return FAIL;
    }
    if (strncmp(buffer, "Hello", 5) != 0) {
        tlog("Unexpected bytes from blobstream_read");
        return FAIL;
    }
    count = blobstream_read(blobstr, buffer, 10);
    if (count != 6) {
        tlog("Unexpected error %d (errno %d) from blobstream_read", (int) count,
             (int) errno);
        return FAIL;
    }
    if (strncmp(buffer, " world", 6) != 0) {
        tlog("Unexpected bytes from blobstream_read");
        return FAIL;
    }
    count = blobstream_read(blobstr, buffer, 10);
    if (count != 0) {
        tlog("Unexpected error %d (errno %d) from blobstream_read", (int) count,
             (int) errno);
        return FAIL;
    }
    blobstream_close(blobstr);
    destroy_async(async);
    return posttest_check(PASS);
}
