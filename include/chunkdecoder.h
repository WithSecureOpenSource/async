#ifndef __CHUNKDECODER__
#define __CHUNKDECODER__

#include "async.h"
#include "bytestream_1.h"
#include "bytestream_2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chunkdecoder chunkdecoder_t;

enum {
    CHUNKDECODER_DETACH_AT_TRAILER,
    CHUNKDECODER_DETACH_AFTER_TRAILER,
    CHUNKDECODER_ADOPT_INPUT,
    CHUNKDECODER_DETACH_AT_FINAL_EXTENSIONS
};

/*
 * Decode a chunk-encoded stream.
 *
 * If 'mode' is CHUNKDECODER_DETACH_AT_TRAILER, stop processing at the
 * beginning of the trailer (whether it is present or not) and leave the
 * final CRLF in the stream. See chunkdecoder_leftover_bytes() below.
 *
 * If 'mode' is CHUNKDECODER_DETACH_AFTER_TRAILER, skip the possible
 * trailer and read out the final CRLF. See
 * chunkdecoder_leftover_bytes() below.
 *
 * If 'mode' is CHUNKDECODER_ADOPT_INPUT, skip the possible trailer and
 * read out the final CRLF. Additionally verify that the underlying
 * stream ends when the chunked encoding does. (Otherwise, an EPROTO is
 * generated instead of an EOF.
 *
 * If 'mode' is CHUNKDECODER_DETACH_AT_FINAL_EXTENSIONS, stop processing
 * at the end of the final 0 chunk size and leave the possible final
 * extensions in the stream. See chunkdecoder_leftover_bytes() below.
 */
chunkdecoder_t *chunk_decode(async_t *async, bytestream_1 stream, int mode);

bytestream_1 chunkdecoder_as_bytestream_1(chunkdecoder_t *decoder);
bytestream_2 chunkdecoder_as_bytestream_2(chunkdecoder_t *decoder);
ssize_t chunkdecoder_read(chunkdecoder_t *decoder, void *buf, size_t count);

/*
 * If 'mode' is CHUNKDECODER_ADOPT_INPUT, closing the chunk decoder also
 * closes the underlying stream.
 *
 * In other (detaching) modes, closing detaches from the underlying
 * stream immediately with no possibility of controlled synchronization.
 * Thus, controlled synchronization requires the exhaustion of the
 * chunkdecoder_t stream before closing.
 */
void chunkdecoder_close(chunkdecoder_t *decoder);

void chunkdecoder_register_callback(chunkdecoder_t *decoder, action_1 action);
void chunkdecoder_unregister_callback(chunkdecoder_t *decoder);

/*
 * In the detaching modes, the chunk decoder object may consume more
 * bytes from the underlying stream than are needed to conclude
 * decoding. Those leftover bytes are accessible between reading an EOF
 * from the decoder and closing it.
 */
size_t chunkdecoder_leftover_size(chunkdecoder_t *decoder);
void *chunkdecoder_leftover_bytes(chunkdecoder_t *decoder);

#ifdef __cplusplus
}
#endif

#endif
