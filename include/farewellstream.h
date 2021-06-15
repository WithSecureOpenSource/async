#ifndef __FAREWELLSTREAM__
#define __FAREWELLSTREAM__

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct farewellstream farewellstream_t;

/*
 * Open a stream wrapper that invokes a callback when the stream is
 * closed. The farewell action is from within the close() method.
 */
farewellstream_t *open_farewellstream(async_t *async, bytestream_1 stream,
                                      action_1 farewell_action);

/*
 * Like open_farewellstream() except that the farewell action is
 * scheduled asynchronously.
 */
farewellstream_t *open_relaxed_farewellstream(async_t *async,
                                              bytestream_1 stream,
                                              action_1 farewell_action);

bytestream_1 farewellstream_as_bytestream_1(farewellstream_t *fwstr);
ssize_t farewellstream_read(farewellstream_t *fwstr, void *buf, size_t count);
void farewellstream_close(farewellstream_t *fwstr);
void farewellstream_register_callback(farewellstream_t *fwstr, action_1 action);
void farewellstream_unregister_callback(farewellstream_t *fwstr);

#ifdef __cplusplus
}
#endif

#endif
