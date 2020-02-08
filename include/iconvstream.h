#ifndef __ICONVSTREAM__
#define __ICONVSTREAM__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iconvstream iconvstream_t;

/* Convert between character encodings using iconv(3). NULL is returned
 * and errno is set if an iconv object cannot be created corresponding
 * to the codes. */
iconvstream_t *open_iconvstream(async_t *async, bytestream_1 source,
                                const char *tocode, const char *fromcode);

bytestream_1 iconvstream_as_bytestream_1(iconvstream_t *stream);
ssize_t iconvstream_read(iconvstream_t *stream, void *buf, size_t count);
void iconvstream_close(iconvstream_t *stream);

void iconvstream_register_callback(iconvstream_t *stream, action_1 action);
void iconvstream_unregister_callback(iconvstream_t *stream);

#ifdef __cplusplus
}
#endif

#endif
