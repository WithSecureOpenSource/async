#include "asynctest-chunkencoder.h"

#include <assert.h>
#include <errno.h>

#include <async/async.h>
#include <async/chunkencoder.h>
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

static int check_chunk_read(ssize_t count, CHUNK_CONTEXT *context)
{
    switch (context->state) {
        case CHUNK_READING_LENGTH:
        case CHUNK_READING_LENGTH_LF:
        case CHUNK_READING_CONTENT:
        case CHUNK_READING_CONTENT_CR:
        case CHUNK_READING_CONTENT_LF:
        case CHUNK_READING_TRAILER_CR:
        case CHUNK_READING_TRAILER_LF:
            if (count <= 0) {
                tlog("Unexpected error %d (errno %d) from chunkencoder",
                     (int) count, (int) errno);
                return 0;
            }
            break;
        case CHUNK_READING_EOF:
            if (count != 0) {
                tlog("Unexpected error %d (errno %d) from chunkencoder",
                     (int) count, (int) errno);
                return 0;
            }
            break;
        default:
            assert(0);
    }
    return 1;
}

static int check_chunk_reading_length(char c, CHUNK_CONTEXT *context)
{
    if (c == '\r')
        context->state = CHUNK_READING_LENGTH_LF;
    else if (c >= '0' && c <= '9')
        context->length = context->length * 16 + c - '0';
    else if (c >= 'A' && c <= 'F')
        context->length = context->length * 16 + c - 'A' + 10;
    else if (c >= 'a' && c <= 'f')
        context->length = context->length * 16 + c - 'a' + 10;
    else {
        tlog("Funny character in chunk length");
        return 0;
    }
    return 1;
}

static int check_chunk_reading_length_lf(char c, CHUNK_CONTEXT *context)
{
    if (c != '\n') {
        tlog("LF expected after chunk length");
        return 0;
    }
    if (context->length == 0)
        context->state = CHUNK_READING_TRAILER_CR;
    else {
        context->state = CHUNK_READING_CONTENT;
        context->chunk_cursor = 0;
    }
    return 1;
}

static int check_chunk_reading_content(char c, CHUNK_CONTEXT *context)
{
    if (c != chunk_data[context->total_cursor++]) {
        tlog("Unexpected character in chunk content");
        return 0;
    }
    if (++context->chunk_cursor >= context->length)
        context->state = CHUNK_READING_CONTENT_CR;
    return 1;
}

static int check_chunk_reading_content_cr(char c, CHUNK_CONTEXT *context)
{
    if (c != '\r') {
        tlog("CR expected after chunk");
        return 0;
    }
    context->state = CHUNK_READING_CONTENT_LF;
    return 1;
}

static int check_chunk_reading_content_lf(char c, CHUNK_CONTEXT *context)
{
    if (c != '\n') {
        tlog("LF expected after chunk");
        return 0;
    }
    context->state = CHUNK_READING_LENGTH;
    context->length = 0;
    return 1;
}

static int check_chunk_reading_trailer_cr(char c, CHUNK_CONTEXT *context)
{
    if (c != '\r') {
        tlog("CR expected after final chunk");
        return 0;
    }
    context->state = CHUNK_READING_TRAILER_LF;
    return 1;
}

static int check_chunk_reading_trailer_lf(char c, CHUNK_CONTEXT *context)
{
    if (c != '\n') {
        tlog("LF expected after final chunk");
        return 0;
    }
    context->state = CHUNK_READING_EOF;
    return 1;
}

static int check_chunk_reading_eof(char c, CHUNK_CONTEXT *context)
{
    tlog("EOF expected after final chunk");
    return 0;
}

VERDICT test_chunkencoder(void)
{
    /* Simplification: assume no extensions or trailer */
    enum {
        MAX_CHUNK = 30,
    };
    async_t *async = make_async();
    stringstream_t *stringstr = open_stringstream(async, chunk_data);
    chunkencoder_t *encoder =
        chunk_encode(async, stringstream_as_bytestream_1(stringstr), MAX_CHUNK);
    CHUNK_CONTEXT context = {
        .state = CHUNK_READING_LENGTH,
    };
    for (;;) {
        char buffer[100];
        ssize_t count = chunkencoder_read(encoder, buffer, sizeof buffer);
        if (!check_chunk_read(count, &context))
            return FAIL;
        if (count == 0)
            break;
        ssize_t i;
        for (i = 0; i < count; i++)
            switch (context.state) {
                case CHUNK_READING_LENGTH:
                    if (!check_chunk_reading_length(buffer[i], &context))
                        return FAIL;
                    break;
                case CHUNK_READING_LENGTH_LF:
                    if (!check_chunk_reading_length_lf(buffer[i], &context))
                        return FAIL;
                    break;
                case CHUNK_READING_CONTENT:
                    if (!check_chunk_reading_content(buffer[i], &context))
                        return FAIL;
                    break;
                case CHUNK_READING_CONTENT_CR:
                    if (!check_chunk_reading_content_cr(buffer[i], &context))
                        return FAIL;
                    break;
                case CHUNK_READING_CONTENT_LF:
                    if (!check_chunk_reading_content_lf(buffer[i], &context))
                        return FAIL;
                    break;
                case CHUNK_READING_TRAILER_CR:
                    if (!check_chunk_reading_trailer_cr(buffer[i], &context))
                        return FAIL;
                    break;
                case CHUNK_READING_TRAILER_LF:
                    if (!check_chunk_reading_trailer_lf(buffer[i], &context))
                        return FAIL;
                    break;
                case CHUNK_READING_EOF:
                    if (!check_chunk_reading_eof(buffer[i], &context))
                        return FAIL;
                    break;
                default:
                    assert(0);
            }
    }
    chunkencoder_close(encoder);
    destroy_async(async);
    return posttest_check(PASS);
}
