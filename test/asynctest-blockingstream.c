#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <async/async.h>
#include <async/blockingstream.h>
#include "asynctest-blockingstream.h"

VERDICT test_blockingstream(void)
{
    int fd = open("/bin/ls", O_RDONLY);
    assert(fd >= 0);
    async_t *async = make_async();
    blockingstream_t *blkstr = open_blockingstream(async, fd);
    uint8_t buffer[100];
    ssize_t count;
    do {
        count = blockingstream_read(blkstr, buffer, sizeof buffer);
        if (count < 0) {
            tlog("Unexpected error %d (errno %d) from blockingstream_read",
                 (int) count, (int) errno);
            return FAIL;
        }
    } while (count > 0);
    blockingstream_close(blkstr);
    destroy_async(async);
    count = read(fd, buffer, sizeof buffer);
    if (count >= 0 || errno != EBADF) {
        tlog("Unexpected value %d (errno %d) from read",
             (int) count, (int) errno);
        return FAIL;
    }
    return posttest_check(PASS);
}
