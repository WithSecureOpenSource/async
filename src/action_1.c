#include <stddef.h>

#include "action_1.h"

#include "async_version.h"

static void __no_action(void *obj)
{
    (void) (obj);
}

action_1 NULL_ACTION_1 = {
    NULL,
    __no_action,
};
