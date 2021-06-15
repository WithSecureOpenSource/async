#include "bytestream_1.h"

#include <fsdyn/fsalloc.h>

#include "async_version.h"

static void close_stream(bytestream_1 *s)
{
    bytestream_1_close(*s);
    fsfree(s);
}

void bytestream_1_close_relaxed(async_t *async, bytestream_1 stream)
{
    bytestream_1 *copy = fsalloc(sizeof *copy);
    *copy = stream;
    async_execute(async, (action_1) { copy, (act_1) close_stream });
}
