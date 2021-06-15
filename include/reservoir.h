#ifndef __RESERVOIR__
#define __RESERVOIR__

#include <stdbool.h>

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct reservoir reservoir_t;

/* A reservoir is a stream wrapper that buffers incoming data until it
 * sees an EOF. */
reservoir_t *open_reservoir(async_t *async, size_t capacity,
                            bytestream_1 stream);
void reservoir_close(reservoir_t *reservoir);

/* Return the number of bytes stored in the reservoir at the moment. */
size_t reservoir_amount(reservoir_t *reservoir);

/* Read in as many bytes as possible from the underlying stream. Return
 * true if the underlying stream has been exhausted. Return false and
 * set errno otherwise. If the capacity has been reached, false is
 * returned and errno is set to ENOSPC.
 *
 * Consider wrapping the underlying stream in a nicestream_t so the
 * reservoir doesn't hog the CPU. */
bool reservoir_fill(reservoir_t *reservoir);

bytestream_1 reservoir_as_bytestream_1(reservoir_t *reservoir);

/* You can start reading from a reservoir even before it has been filled
 * up. Reading frees up capacity and decreases reservoir_amount(). It
 * also allows you to call reservoir_fill() again after the capacity has
 * been reached. */
ssize_t reservoir_read(reservoir_t *reservoir, void *buf, size_t count);
void reservoir_close(reservoir_t *reservoir);

void reservoir_register_callback(reservoir_t *reservoir, action_1 action);
void reservoir_unregister_callback(reservoir_t *reservoir);

#ifdef __cplusplus
}
#endif

#endif
