#include "asynctest-chunkdecoder.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <async/async.h>
#include <async/blobstream.h>
#include <async/chunkdecoder.h>
#include <async/chunkencoder.h>
#include <async/concatstream.h>
#include <async/queuestream.h>
#include <async/stringstream.h>

static const char *chunk_data =
    "SMS Prinzregent Luitpold was the fifth and "
    "final vessel of the Kaiser class of battleships of the Imperial"
    " German Navy. Prinzregent Luitpold's keel was laid in October 1910"
    " at the Germaniawerft dockyard in Kiel. She was launched on 17"
    " February 1912 and was commissioned into the navy on 19 August 1913."
    " Prinzregent Luitpold was assigned to the III Battle Squadron of the"
    " High Seas Fleet for the majority of her career; in December 1916,"
    " she was transferred to the IV Battle Squadron. Along with her four"
    " sister ships, Kaiser, Friedrich der Grosse, Kaiserin, and König"
    " Albert, Prinzregent Luitpold participated in all of the major fleet"
    " operations of World War I, including the Battle of Jutland on 31"
    " May – 1 June 1916. The ship was also involved in Operation Albion,"
    " an amphibious assault on the Russian-held islands in the Gulf of"
    " Riga, in late 1917.";

static const char *trailer = "One: one\r\n"
                             "Two: one \r\n"
                             "\ttwo\r\n"
                             "Three: three\r\n"
                             "\r\n";

enum {
    CHUNK_READING_LENGTH,
    CHUNK_READING_LENGTH_LF,
    CHUNK_READING_CONTENT,
    CHUNK_READING_CONTENT_CR,
    CHUNK_READING_CONTENT_LF,
    CHUNK_READING_TRAILER_CR,
    CHUNK_READING_TRAILER_LF,
    CHUNK_READING_EOF
};

typedef struct {
    int state;
    size_t length, chunk_cursor, total_cursor;
} CHUNK_CONTEXT;

static VERDICT test_skip_null_trailer(size_t chunk_size, size_t read_size)
{
    async_t *async = make_async();
    stringstream_t *stringstr = open_stringstream(async, chunk_data);
    chunkencoder_t *encoder =
        chunk_encode(async, stringstream_as_bytestream_1(stringstr),
                     chunk_size);
    chunkdecoder_t *decoder =
        chunk_decode(async, chunkencoder_as_bytestream_1(encoder),
                     CHUNKDECODER_ADOPT_INPUT);
    size_t data_length = strlen(chunk_data);
    size_t total_count = 0;
    char buffer[read_size];
    ssize_t count;
    while (total_count < data_length) {
        count = chunkdecoder_read(decoder, buffer, sizeof buffer);
        if (count < 0) {
            tlog("Unexpected error %d (errno %d) from chunkdecoder",
                 (int) count, (int) errno);
            return FAIL;
        }
        if (count == 0) {
            tlog("Unexpected EOF after reading %u (out of %u) bytes "
                 "from chunkdecoder",
                 (int) total_count, (int) data_length);
            return FAIL;
        }
        if (total_count + count > data_length) {
            tlog("Too many bytes returned by chunkdecoder");
            return FAIL;
        }
        if (memcmp(chunk_data + total_count, buffer, count)) {
            tlog("Bad bytes returned by chunkdecoder");
            return FAIL;
        }
        total_count += count;
    }
    count = chunkdecoder_read(decoder, buffer, sizeof buffer);
    if (count < 0) {
        tlog("Unexpected error %d (errno %d) from chunkdecoder", (int) count,
             (int) errno);
        return FAIL;
    }
    if (count > 0) {
        tlog("Too many bytes returned by chunkdecoder");
        return FAIL;
    }
    chunkdecoder_close(decoder);
    destroy_async(async);
    return PASS;
}

static VERDICT test_skip_real_trailer(size_t chunk_size, size_t read_size)
{
    async_t *async = make_async();
    stringstream_t *stringstr = open_stringstream(async, chunk_data);
    chunkencoder_t *encoder =
        chunk_encode_2(async, stringstream_as_bytestream_1(stringstr),
                       chunk_size, 1);
    stringstr = open_stringstream(async, trailer);
    concatstream_t *conc =
        concatenate_two_streams(async, chunkencoder_as_bytestream_1(encoder),
                                stringstream_as_bytestream_1(stringstr));
    chunkdecoder_t *decoder =
        chunk_decode(async, concatstream_as_bytestream_1(conc),
                     CHUNKDECODER_ADOPT_INPUT);
    size_t data_length = strlen(chunk_data);
    size_t total_count = 0;
    char buffer[read_size];
    ssize_t count;
    while (total_count < data_length) {
        count = chunkdecoder_read(decoder, buffer, sizeof buffer);
        if (count < 0) {
            tlog("Unexpected error %d (errno %d) from chunkdecoder",
                 (int) count, (int) errno);
            return FAIL;
        }
        if (count == 0) {
            tlog("Unexpected EOF after reading %u (out of %u) bytes "
                 "from chunkdecoder",
                 (int) total_count, (int) data_length);
            return FAIL;
        }
        if (total_count + count > data_length) {
            tlog("Too many bytes returned by chunkdecoder");
            return FAIL;
        }
        if (memcmp(chunk_data + total_count, buffer, count)) {
            tlog("Bad bytes returned by chunkdecoder");
            return FAIL;
        }
        total_count += count;
    }
    count = chunkdecoder_read(decoder, buffer, sizeof buffer);
    if (count < 0) {
        tlog("Unexpected error %d (errno %d) from chunkdecoder", (int) count,
             (int) errno);
        return FAIL;
    }
    if (count > 0) {
        tlog("Too many bytes returned by chunkdecoder");
        return FAIL;
    }
    chunkdecoder_close(decoder);
    destroy_async(async);
    return PASS;
}

static VERDICT check_trailer(bytestream_1 stream, const char *trailer)
{
    size_t trailer_size = strlen(trailer);
    char trailer_buffer[trailer_size + 1];
    size_t trailer_count = 0;
    while (trailer_count < trailer_size + 1) {
        ssize_t count =
            bytestream_1_read(stream, trailer_buffer + trailer_count,
                              trailer_size + 1 - trailer_count);
        if (count < 0) {
            tlog("Unexpected error %d (errno %d) from trailer stream",
                 (int) count, (int) errno);
            return FAIL;
        }
        if (count == 0)
            break;
        trailer_count += count;
    }
    if (trailer_count != trailer_size) {
        tlog("Bad trailer size");
        return FAIL;
    }
    if (memcmp(trailer_buffer, trailer, trailer_size)) {
        tlog("Bad trailer");
        return FAIL;
    }
    return PASS;
}

static VERDICT test_read_real_trailer(size_t chunk_size, size_t read_size)
{
    async_t *async = make_async();
    stringstream_t *stringstr = open_stringstream(async, chunk_data);
    chunkencoder_t *encoder =
        chunk_encode_2(async, stringstream_as_bytestream_1(stringstr),
                       chunk_size, 1);
    stringstr = open_stringstream(async, trailer);
    queuestream_t *q = make_queuestream(async);
    queuestream_enqueue(q, chunkencoder_as_bytestream_1(encoder));
    queuestream_enqueue(q, stringstream_as_bytestream_1(stringstr));
    queuestream_terminate(q);
    chunkdecoder_t *decoder =
        chunk_decode(async, queuestream_as_bytestream_1(q),
                     CHUNKDECODER_DETACH_AT_TRAILER);
    size_t data_length = strlen(chunk_data);
    size_t total_count = 0;
    char buffer[read_size];
    ssize_t count;
    while (total_count < data_length) {
        count = chunkdecoder_read(decoder, buffer, sizeof buffer);
        if (count < 0) {
            tlog("Unexpected error %d (errno %d) from chunkdecoder",
                 (int) count, (int) errno);
            return FAIL;
        }
        if (count == 0) {
            tlog("Unexpected EOF after reading %u (out of %u) bytes "
                 "from chunkdecoder",
                 (int) total_count, (int) data_length);
            return FAIL;
        }
        if (total_count + count > data_length) {
            tlog("Too many bytes returned by chunkdecoder");
            return FAIL;
        }
        if (memcmp(chunk_data + total_count, buffer, count)) {
            tlog("Bad bytes returned by chunkdecoder");
            return FAIL;
        }
        total_count += count;
    }
    count = chunkdecoder_read(decoder, buffer, sizeof buffer);
    if (count < 0) {
        tlog("Unexpected error %d (errno %d) from chunkdecoder", (int) count,
             (int) errno);
        return FAIL;
    }
    if (count > 0) {
        tlog("Too many bytes returned by chunkdecoder");
        return FAIL;
    }
    blobstream_t *leftovers =
        copy_blobstream(async, chunkdecoder_leftover_bytes(decoder),
                        chunkdecoder_leftover_size(decoder));
    queuestream_push(q, blobstream_as_bytestream_1(leftovers));
    chunkdecoder_close(decoder);
    if (check_trailer(queuestream_as_bytestream_1(q), trailer) == FAIL)
        return FAIL;
    queuestream_close(q);
    destroy_async(async);
    return PASS;
}

static VERDICT check_asterisk_after_trailer(bytestream_1 stream)
{
    char c;
    ssize_t count = bytestream_1_read(stream, &c, 1);
    if (count < 0) {
        tlog("Unexpected error %d (errno %d) when reading after trailer",
             (int) count, (int) errno);
        return FAIL;
    }
    if (count == 0) {
        tlog("Unexpected EOF after trailer");
        return FAIL;
    }
    if (c != '*') {
        tlog("Unexpected content after trailer");
        return FAIL;
    }
    return PASS;
}

static VERDICT test_detach_after_trailer(size_t chunk_size, size_t read_size)
{
    async_t *async = make_async();
    stringstream_t *stringstr = open_stringstream(async, chunk_data);
    chunkencoder_t *encoder =
        chunk_encode_2(async, stringstream_as_bytestream_1(stringstr),
                       chunk_size, 1);
    stringstr = open_stringstream(async, trailer);
    queuestream_t *q = make_queuestream(async);
    queuestream_enqueue(q, chunkencoder_as_bytestream_1(encoder));
    queuestream_enqueue(q, stringstream_as_bytestream_1(stringstr));
    stringstr = open_stringstream(async, "*");
    queuestream_enqueue(q, stringstream_as_bytestream_1(stringstr));
    queuestream_terminate(q);
    chunkdecoder_t *decoder =
        chunk_decode(async, queuestream_as_bytestream_1(q),
                     CHUNKDECODER_DETACH_AFTER_TRAILER);
    size_t data_length = strlen(chunk_data);
    size_t total_count = 0;
    char buffer[read_size];
    ssize_t count;
    while (total_count < data_length) {
        count = chunkdecoder_read(decoder, buffer, sizeof buffer);
        if (count < 0) {
            tlog("Unexpected error %d (errno %d) from chunkdecoder",
                 (int) count, (int) errno);
            return FAIL;
        }
        if (count == 0) {
            tlog("Unexpected EOF after reading %u (out of %u) bytes "
                 "from chunkdecoder",
                 (int) total_count, (int) data_length);
            return FAIL;
        }
        if (total_count + count > data_length) {
            tlog("Too many bytes returned by chunkdecoder");
            return FAIL;
        }
        if (memcmp(chunk_data + total_count, buffer, count)) {
            tlog("Bad bytes returned by chunkdecoder");
            return FAIL;
        }
        total_count += count;
    }
    count = chunkdecoder_read(decoder, buffer, sizeof buffer);
    if (count < 0) {
        tlog("Unexpected error %d (errno %d) from chunkdecoder", (int) count,
             (int) errno);
        return FAIL;
    }
    if (count > 0) {
        tlog("Too many bytes returned by chunkdecoder");
        return FAIL;
    }
    blobstream_t *leftovers =
        copy_blobstream(async, chunkdecoder_leftover_bytes(decoder),
                        chunkdecoder_leftover_size(decoder));
    queuestream_push(q, blobstream_as_bytestream_1(leftovers));
    chunkdecoder_close(decoder);
    if (check_asterisk_after_trailer(queuestream_as_bytestream_1(q)) == FAIL)
        return FAIL;
    queuestream_close(q);
    destroy_async(async);
    return PASS;
}

VERDICT test_chunkdecoder(void)
{
    if (test_skip_null_trailer(20, 30) == FAIL ||
        test_skip_null_trailer(30, 20) == FAIL ||
        test_skip_null_trailer(1, 10000) == FAIL ||
        test_skip_null_trailer(10000, 1) == FAIL ||
        test_skip_real_trailer(30, 20) == FAIL ||
        test_read_real_trailer(30, 20) == FAIL ||
        test_detach_after_trailer(30, 20) == FAIL)
        return FAIL;
    return posttest_check(PASS);
}
