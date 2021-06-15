#include "asynctest-probestream.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <async/async.h>
#include <async/farewellstream.h>
#include <async/probestream.h>
#include <async/stringstream.h>

void read_probe_cb(void *obj, const void *buf, size_t buf_size,
                   ssize_t return_value)
{
    --*(int *) obj;
}

void close_probe_cb(void *obj)
{
    --*(int *) obj;
}

VERDICT test_probestream(void)
{
    async_t *async = make_async();
    stringstream_t *stringstr = open_stringstream(async, "Hello world");
    farewellstream_t *up =
        open_farewellstream(async, stringstream_as_bytestream_1(stringstr),
                            NULL_ACTION_1);
    int s_test_shots = 2;
    probestream_t *probe = open_probestream(async, &s_test_shots,
                                            farewellstream_as_bytestream_1(up),
                                            close_probe_cb, read_probe_cb);
    farewellstream_t *down =
        open_farewellstream(async, probestream_as_bytestream_1(probe),
                            NULL_ACTION_1);
    char buffer[100];
    ssize_t count;
    count = farewellstream_read(down, buffer, 20);
    if (count != 11) {
        tlog("Unexpected bytes from stringstream_read (%d != 11)", (int) count);
        return FAIL;
    }
    farewellstream_close(down);
    if (s_test_shots != 0) {
        tlog("Something's going on down at the shooting range "
             "(Remaining shots: %d)",
             s_test_shots);
        return FAIL;
    }
    destroy_async(async);
    return posttest_check(PASS);
}
