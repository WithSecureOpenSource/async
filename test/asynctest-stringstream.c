#include "asynctest-stringstream.h"

#include <errno.h>
#include <string.h>

#include <async/async.h>
#include <async/stringstream.h>

VERDICT test_stringstream(void)
{
    async_t *async = make_async();
    stringstream_t *stringstr = open_stringstream(async, "Hello world");
    char buffer[100];
    ssize_t count;
    count = stringstream_read(stringstr, buffer, 5);
    if (count != 5) {
        tlog("Unexpected error %d (errno %d) from stringstream_read",
             (int) count, (int) errno);
        return FAIL;
    }
    if (strncmp(buffer, "Hello", 5) != 0) {
        tlog("Unexpected bytes from stringstream_read");
        return FAIL;
    }
    count = stringstream_read(stringstr, buffer, 10);
    if (count != 6) {
        tlog("Unexpected error %d (errno %d) from stringstream_read",
             (int) count, (int) errno);
        return FAIL;
    }
    if (strncmp(buffer, " world", 6) != 0) {
        tlog("Unexpected bytes from stringstream_read");
        return FAIL;
    }
    count = stringstream_read(stringstr, buffer, 10);
    if (count != 0) {
        tlog("Unexpected error %d (errno %d) from stringstream_read",
             (int) count, (int) errno);
        return FAIL;
    }
    stringstream_close(stringstr);
    destroy_async(async);
    return posttest_check(PASS);
}
