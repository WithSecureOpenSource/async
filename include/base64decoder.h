#ifndef __BASE64DECODER__
#define __BASE64DECODER__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct base64decoder base64decoder_t;

/* Decode a base64-encoded stream. Specifying -1 as pos62 and pos63
 * chooses the defaults ('+' and '/', respectively). */
base64decoder_t *base64_decode(async_t *async, bytestream_1 stream, char pos62,
                               char pos63);

bytestream_1 base64decoder_as_bytestream_1(base64decoder_t *decoder);
ssize_t base64decoder_read(base64decoder_t *decoder, void *buf, size_t count);
void base64decoder_close(base64decoder_t *decoder);

void base64decoder_register_callback(base64decoder_t *decoder, action_1 action);
void base64decoder_unregister_callback(base64decoder_t *decoder);

#ifdef __cplusplus
}
#endif

#endif
