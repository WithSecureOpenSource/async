#pragma once
static __attribute__((constructor)) void ASYNC_VERSION()
{
    extern const char *async_version_tag;
    if (!*async_version_tag)
        async_version_tag++;
}
