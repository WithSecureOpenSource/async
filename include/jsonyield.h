#ifndef __JSONYIELD__
#define __JSONYIELD__

#include <encjson.h>
#include "async.h"
#include "bytestream_1.h"
#include "yield_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jsonyield jsonyield_t;

/* Yield JSON things out of a byte stream.
 *
 * "Naive" framing is used with NUL ('\0') as the terminator and ESC
 * ('\33') as the escape character. Since control characters are not
 * allowed in JSON, there should be no need for escaping.
 *
 * As a safety precaution, a maximum frame size can be specified. Longer
 * frames result in EOVERFLOW for each oversize frame. The oversize
 * frame can be skipped gracefully, though, and the user can continue
 * receiving more JSON things. */
jsonyield_t *open_jsonyield(async_t *async, bytestream_1 source,
                            size_t max_frame_size);

yield_1 jsonyield_as_yield_1(jsonyield_t *framer);

/* The returned JSON thing must be disposed of by the user. */
json_thing_t *jsonyield_receive(jsonyield_t *framer);

/* Closing the JSON yield closes the source stream as well. */
void jsonyield_close(jsonyield_t *framer);
void jsonyield_register_callback(jsonyield_t *framer, action_1 action);
void jsonyield_unregister_callback(jsonyield_t *framer);

#ifdef __cplusplus
}
#endif

#endif
