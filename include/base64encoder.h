#ifndef __BASE64ENCODER__
#define __BASE64ENCODER__

#include <stdbool.h>

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct base64encoder base64encoder_t;

/* Base64-encode a stream. Specifying -1 as pos62, pos63 and padchar
 * chooses the defaults ('+', '/' and '=', respectively). If pad is
 * true, zero, one or two padding characters are appended to the
 * encoding. */
base64encoder_t *base64_encode(async_t *async, bytestream_1 stream, char pos62,
                               char pos63, bool pad, char padchar);

bytestream_1 base64encoder_as_bytestream_1(base64encoder_t *encoder);
ssize_t base64encoder_read(base64encoder_t *encoder, void *buf, size_t count);
void base64encoder_close(base64encoder_t *encoder);

void base64encoder_register_callback(base64encoder_t *encoder, action_1 action);
void base64encoder_unregister_callback(base64encoder_t *encoder);

#ifdef __cplusplus
}
#endif

#endif
