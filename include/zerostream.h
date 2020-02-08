#ifndef __ZEROSTREAM__
#define __ZEROSTREAM__

#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The zero stream's read method returns an unending stream of zeros.
 * The object is analogous to /dev/zero.
 */
extern const bytestream_1 zerostream;

#ifdef __cplusplus
}
#endif

#endif
