#include "asynctest-pausestream.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <async/async.h>
#include <async/pausestream.h>

off_t pausestr_cb(void *ctx)
{
    off_t *limit = (off_t *) ctx;
    return *limit;
}

VERDICT test_pausestream(void)
{
    int fd = open("/bin/ls", O_RDONLY);
    assert(fd >= 0);
    struct stat st;
    fstat(fd, &st);
    async_t *async = make_async();
    pausestream_t *pausestr = open_pausestream(async, fd);
    char *buffer;
    ssize_t count;
    off_t limit = 0;
    buffer = malloc(st.st_size);

    errno = 0;
    count = pausestream_read(pausestr, buffer, 5);
    if (count != -1 || errno != EAGAIN) {
        tlog("Unexpected result %d, errno %d from pausestream_read "
             "(expected -1, EAGAIN)",
             (int) count, (int) errno);
        return FAIL;
    }

    pausestream_limit_cb_1 limit_cb = { &limit, pausestr_cb };
    pausestream_set_limit_callback(pausestr, limit_cb);
    errno = 0;
    count = pausestream_read(pausestr, buffer, 5);
    if (count != -1 || errno != EAGAIN) {
        tlog("Unexpected result %d, errno %d from pausestream_read "
             "(expected -1, EAGAIN)",
             (int) count, (int) errno);
        return FAIL;
    }

    limit = 5;
    errno = 0;
    count = pausestream_read(pausestr, buffer, 10);
    if (count != 5 || errno != 0) {
        tlog("Unexpected result %d, errno %d from pausestream_read "
             "(expected 5, 0)",
             (int) count, (int) errno);
        return FAIL;
    }

    limit = 5;
    errno = 0;
    count = pausestream_read(pausestr, buffer, 10);
    if (count != -1 || errno != EAGAIN) {
        tlog("Unexpected result %d, errno %d from pausestream_read "
             "(expected -1, EAGAIN)",
             (int) count, (int) errno);
        return FAIL;
    }

    limit = -1;
    errno = 0;
    count = pausestream_read(pausestr, buffer, 10);
    if (count != 10 || errno != 0) {
        tlog("Unexpected result %d, errno %d from pausestream_read "
             "(expected 10, 0)",
             (int) count, (int) errno);
        return FAIL;
    }

    limit = -1;
    errno = 0;
    count = pausestream_read(pausestr, buffer, st.st_size);
    if (count != (st.st_size - 5 - 10) || errno != 0) {
        tlog("Unexpected result %d, errno %d from pausestream_read "
             "(expected %d, 0)",
             (int) count, (int) errno, (int) (st.st_size - 5 - 10));
        return FAIL;
    }

    pausestream_close(pausestr);
    destroy_async(async);
    close(fd);
    free(buffer);
    return posttest_check(PASS);
}
