#ifndef __NAIVEFRAMER__
#define __NAIVEFRAMER__

#include "async.h"
#include "bytestream_1.h"
#include "yield_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct naiveframer naiveframer_t;

/* Break the given source byte stream into frames, which are themselves
 * byte streams.
 *
 * The frames in source must be terminated with a terminator byte. An
 * escape byte may be specified that can be used to embed any byte into
 * the frame payload. If there is no escape byte, specify the terminator
 * byte itself as the escape byte. */
naiveframer_t *open_naiveframer(async_t *async, bytestream_1 source,
                                uint8_t terminator, uint8_t escape);

yield_1 naiveframer_as_yield_1(naiveframer_t *framer);

/* The returned frame is normally closed by the user. However, the
 * framer retains ownership to the frame; if the frame is still open
 * when the framer is closed, the framer closes it. */
bytestream_1 *naiveframer_receive(naiveframer_t *framer);

/* Closing the naive framer closes the source stream as well. */
void naiveframer_close(naiveframer_t *framer);
void naiveframer_register_callback(naiveframer_t *framer, action_1 action);
void naiveframer_unregister_callback(naiveframer_t *framer);

#ifdef __cplusplus
}
#endif

#endif
