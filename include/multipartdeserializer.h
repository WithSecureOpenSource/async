#ifndef __MULTIPARTDESERIALIZER__
#define __MULTIPARTDESERIALIZER__

#include "async.h"
#include "bytestream_1.h"
#include "yield_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct multipartdeserializer multipartdeserializer_t;

/* Break the given RFC 2046 multipart body stream into parts, which
 * are themselves byte streams.
 */
multipartdeserializer_t *open_multipartdeserializer(async_t *async,
                                                    bytestream_1 source,
                                                    const char *boundary);

yield_1 multipartdeserializer_as_yield_1(multipartdeserializer_t *deserializer);

/* The returned frame is normally closed by the user. However, the
 * deserializer retains ownership to the frame; if the frame is still open
 * when the deserializer is closed, the deserializer closes it. */
bytestream_1 *multipartdeserializer_receive(
    multipartdeserializer_t *deserializer);

/* Closing the multipart deserializer closes the source stream as well. */
void multipartdeserializer_close(multipartdeserializer_t *deserializer);
void multipartdeserializer_register_callback(
    multipartdeserializer_t *deserializer, action_1 action);
void multipartdeserializer_unregister_callback(
    multipartdeserializer_t *deserializer);

#ifdef __cplusplus
}
#endif

#endif
