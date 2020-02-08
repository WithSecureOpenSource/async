#ifndef __EMPTYSTREAM__
#define __EMPTYSTREAM__

#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The empty stream's read method always returns 0 (end of file). The
 * object is analogous to /dev/null.
 */
extern const bytestream_1 emptystream;

#ifdef __cplusplus
}
#endif

#endif
