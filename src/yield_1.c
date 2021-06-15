#include "yield_1.h"

#include <fsdyn/fsalloc.h>

#include "async_version.h"

static void close_yield(yield_1 *s)
{
    yield_1_close(*s);
    fsfree(s);
}

void yield_1_close_relaxed(async_t *async, yield_1 yield)
{
    yield_1 *copy = fsalloc(sizeof *copy);
    *copy = yield;
    async_execute(async, (action_1) { copy, (act_1) close_yield });
}
