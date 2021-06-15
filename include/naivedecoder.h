#ifndef __NAIVEDECODER__
#define __NAIVEDECODER__

#include "async.h"
#include "bytestream_1.h"
#include "bytestream_2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct naivedecoder naivedecoder_t;

enum {
    NAIVEDECODER_DETACH,
    NAIVEDECODER_ADOPT_INPUT,
};

/*
 * Decode a source stream that is terminated with an EOF marker byte.
 * Escaping with a prefix byte is supported. The EOF marker byte is read
 * off the underlying stream.
 *
 * If 'mode' is NAIVEDECODER_DETACH, leave the underlying stream open
 * after the naive stream is closed. See naivedecoder_leftover_bytes()
 * below.
 *
 * If 'mode' is NAIVEDECODER_ADOPT_INPUT, verify that the underlying
 * stream ends when the naive stream does. (Otherwise, an EPROTO is
 * generated instead of an EOF.
 */
naivedecoder_t *naive_decode(async_t *async, bytestream_1 source, int mode,
                             uint8_t terminator, uint8_t escape);

bytestream_1 naivedecoder_as_bytestream_1(naivedecoder_t *decoder);
bytestream_2 naivedecoder_as_bytestream_2(naivedecoder_t *decoder);
ssize_t naivedecoder_read(naivedecoder_t *decoder, void *buf, size_t count);

/*
 * If 'mode' is NAIVEDECODER_ADOPT_INPUT, closing the naive decoder also
 * closes the underlying stream.
 *
 * If 'mode' is NAIVEDECODER_DETACH, closing detaches from the underlying
 * stream immediately with no possibility of controlled synchronization.
 * Thus, controlled synchronization requires the exhaustion of the
 * naivedecoder_t stream before closing.
 */
void naivedecoder_close(naivedecoder_t *decoder);

void naivedecoder_register_callback(naivedecoder_t *decoder, action_1 action);
void naivedecoder_unregister_callback(naivedecoder_t *decoder);

/*
 * In the NAIVEDECODER_DETACH mode, the naive decoder object may consume
 * more bytes from the underlying stream than are needed to conclude
 * decoding. Those leftover bytes are accessible between reading an EOF
 * from the decoder and closing it.
 */
size_t naivedecoder_leftover_size(naivedecoder_t *decoder);
void *naivedecoder_leftover_bytes(naivedecoder_t *decoder);

#ifdef __cplusplus
}
#endif

#endif
