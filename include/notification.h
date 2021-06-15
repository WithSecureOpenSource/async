#ifndef __NOTIFICATION__
#define __NOTIFICATION__

#include "action_1.h"
#include "async.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct notification notification_t;

/* Create a notification object. The given action is scheduled for
 * execution at least once after issue_notification() is called. May
 * return NULL and set errno. */
notification_t *make_notification(async_t *async, action_1 action);

/* The destructor. */
void destroy_notification(notification_t *notification);

/* Trigger the invocation of the associated action. Multiple
 * notifications may be merged into one.
 *
 * The function may be called safely from a signal handler or separate
 * thread. */
void issue_notification(notification_t *notification);

#ifdef __cplusplus
}
#endif

#endif
