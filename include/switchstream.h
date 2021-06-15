#ifndef __SWITCHSTREAM__
#define __SWITCHSTREAM__

#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct switchstream switchstream_t;

/*
 * A switch stream is a transparent wrapper around another stream.
 * You can reattach it to another underlying stream on the fly.
 */
switchstream_t *open_switch_stream(async_t *async, bytestream_1 stream);

/* Detach from the previous stream and attach a new stream. The
 * previously attached stream is returned. The new stream's callback is
 * immediately registered. The old stream's callback is left intact. */
bytestream_1 switchstream_reattach(switchstream_t *swstr, bytestream_1 stream);

bytestream_1 switchstream_as_bytestream_1(switchstream_t *swstr);
ssize_t switchstream_read(switchstream_t *swstr, void *buf, size_t count);

/* Closing a switch stream closes the underlying stream as well. */
void switchstream_close(switchstream_t *swstr);
void switchstream_register_callback(switchstream_t *swstr, action_1 action);
void switchstream_unregister_callback(switchstream_t *swstr);

#ifdef __cplusplus
}
#endif

#endif
