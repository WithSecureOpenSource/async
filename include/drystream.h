#ifndef __DRYSTREAM__
#define __DRYSTREAM__

#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The dry stream's read method always returns -1/EAGAIN (no data yet).
 */
extern const bytestream_1 drystream;

#ifdef __cplusplus
}
#endif

#endif
