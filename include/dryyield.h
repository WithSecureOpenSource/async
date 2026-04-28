#pragma once

#include "yield_1.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The dry stream's receive method always returns NULL/EAGAIN (no data yet).
 */
extern const yield_1 dryyield;

#ifdef __cplusplus
}
#endif
