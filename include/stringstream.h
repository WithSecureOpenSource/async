#ifndef __STRINGSTREAM__
#define __STRINGSTREAM__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct stringstream stringstream_t;

/*
 * Return a bytestream that returns the bytes of the given C string. The
 * string must stay in existence until the stream is closed.
 */
stringstream_t *open_stringstream(async_t *async, const char *string);

/*
 * Like open_stringstream() but string is copied by copy_stringstream().
 */
stringstream_t *copy_stringstream(async_t *async, const char *string);

/*
 * Like open_stringstream() but close_action is performed when the stream
 * is closed.
 */
stringstream_t *adopt_stringstream(async_t *async, const char *string,
                                   action_1 close_action);

bytestream_1 stringstream_as_bytestream_1(stringstream_t *strstr);
ssize_t stringstream_read(stringstream_t *strstr, void *buf, size_t count);
void stringstream_close(stringstream_t *strstr);
void stringstream_register_callback(stringstream_t *strstr, action_1 action);
void stringstream_unregister_callback(stringstream_t *strstr);

#ifdef __cplusplus
}
#endif

#endif
