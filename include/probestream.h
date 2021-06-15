#ifndef __PROBESTREAM__
#define __PROBESTREAM__

#include <sys/types.h>

#include "action_1.h"
#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct probestream probestream_t;
typedef void (*probestream_read_probe_cb_t)(void *obj, const void *buf,
                                            size_t buf_size,
                                            ssize_t return_value);
typedef void (*probestream_close_probe_cb_t)(void *obj);

/*
 * Opens a probestream. This can be used to trace reading / closing activities
 * for example to help in debugging reading / closing issues.
 * Callbacks for both read and close events can be provided.
 * The callbacks are both issued after the relative operation has been
 * performed.
 */
probestream_t *open_probestream(async_t *async, void *obj, bytestream_1 source,
                                probestream_close_probe_cb_t close_cb,
                                probestream_read_probe_cb_t read_cb);
bytestream_1 probestream_as_bytestream_1(probestream_t *probestr);

#ifdef __cplusplus
}
#endif

#endif
