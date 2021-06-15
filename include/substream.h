#ifndef __SUBSTREAM__
#define __SUBSTREAM__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct substream substream_t;

enum {
    SUBSTREAM_NO_END,
    SUBSTREAM_FAST_FORWARD,
    SUBSTREAM_CLOSE_AT_END,
    SUBSTREAM_DETACHED
};

/*
 * A substream discards the initial and final bytes of the underlying
 * stream.
 *
 * If "mode" is SUBSTREAM_NO_END, no bytes are discarded from
 * the end ("end" is ignored).
 *
 * If "mode" is SUBSTREAM_FAST_FORWARD, the underlying stream is read to
 * EOF before the substream declares an EOF.
 *
 * If "mode" is SUBSTREAM_CLOSE_AT_END, the underlying stream is closed
 * when "end" is reached.
 *
 * If "mode" is SUBSTREAM_DETACHED, the underlying stream is not read
 * past "end". Closing the substream will not close the underlying
 * stream. The underlying stream is left at the position of the last
 * read where reading can be continued normally.
 *
 * Consider wrapping the underlying stream in a nicestream_t as a small
 * read may translate into a large and slow read of the underlying
 * stream.
 */
substream_t *make_substream(async_t *async, bytestream_1 stream, int mode,
                            size_t begin, size_t end);

bytestream_1 substream_as_bytestream_1(substream_t *substr);
ssize_t substream_read(substream_t *substr, void *buf, size_t count);
void substream_close(substream_t *substr);
void substream_register_callback(substream_t *substr, action_1 action);
void substream_unregister_callback(substream_t *substr);

#ifdef __cplusplus
}
#endif

#endif
