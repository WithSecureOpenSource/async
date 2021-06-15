#pragma once

#include <stdbool.h>

#include "async.h"

typedef struct alock alock_t;

/* Create an object to asynchronously manage an advisory lock on a
 * file using flock(2) in a subprocess. The file to lock is opened
 * exclusively in the subprocess on the first lock or unlock
 * operation. */
alock_t *make_alock(async_t *async, const char *path, action_1 post_fork_cb);
void destroy_alock(alock_t *alock);
/*
 * Lock or unlock the underlying file. On failure, false is returned
 * and errno is set. If a previous operation is still pending, the
 * function fails and sets errno to EAGAIN. An operation remains
 * pending until the application has received a lock state
 * notification.
 */
bool alock_lock(alock_t *alock);
bool alock_unlock(alock_t *alock);
/*
 * Check for a lock state notification. Notifications are triggered by
 * calls to 'alock_lock' or 'alock_unlock'. On failure, false is
 * returned and errno is set. On success, the current lock state is
 * stored in the 'locked' argument.
 */
bool alock_check(alock_t *alock, bool *locked);

void alock_register_callback(alock_t *alock, action_1 action);
void alock_unregister_callback(alock_t *alock);
