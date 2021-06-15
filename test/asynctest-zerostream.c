#include "asynctest-zerostream.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <async/zerostream.h>

VERDICT test_zerostream(void)
{
    uint8_t buffer[100];
    int i, k;
    for (k = 0; k < 2; k++) {
        for (i = 0; i < 10; i++) {
            memset(buffer, 'x', sizeof buffer);
            ssize_t count =
                bytestream_1_read(zerostream, buffer, sizeof buffer);
            if (count != sizeof buffer) {
                tlog("Unexpected count: %d", (int) count);
                return FAIL;
            }
            int j;
            for (j = 0; j < sizeof buffer; j++)
                if (buffer[j] != 0) {
                    tlog("Nonzero content");
                    return FAIL;
                }
        }
        bytestream_1_close(zerostream);
    }
    return posttest_check(PASS);
}
