#ifndef __DESERIALIZER__
#define __DESERIALIZER__

#include "async.h"
#include "bytestream_1.h"
#include "bytestream_2.h"
#include "yield_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct deserializer deserializer_t;

typedef bytestream_2 (*decoder_factory_t)(void *factory_obj,
                                          bytestream_1 source);

deserializer_t *open_deserializer(async_t *async, bytestream_1 source,
                                  decoder_factory_t factory, void *factory_obj);

yield_1 deserializer_as_yield_1(deserializer_t *deserializer);

bytestream_1 *deserializer_receive(deserializer_t *deserializer);

void deserializer_close(deserializer_t *deserializer);
void deserializer_register_callback(deserializer_t *deserializer,
                                    action_1 action);
void deserializer_unregister_callback(deserializer_t *deserializer);

#ifdef __cplusplus
}
#endif

#endif
