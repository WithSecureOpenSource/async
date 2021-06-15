#pragma once

#include <stdbool.h>

#include <fsdyn/list.h>

#include "async.h"
#include "bytestream_1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct subprocess subprocess_t;

/*
 * Create a subprocess using fork(2).
 *
 * The given post-fork callback is run in the subprocess
 * synchronously, i.e., not from the main loop of the given async
 * object, and can be used to execute a program with exec(3). If the
 * subprocess uses the given async object, the behaviour is undefined.
 * If the subprocess uses exit(3) to terminate, the behaviour is
 * undefined. This function acquires ownership of the list of file
 * descriptors argument.
 */
subprocess_t *open_subprocess(async_t *async, list_t *keep_fds,
                              bool capture_stdout, bool capture_stderr,
                              action_1 post_fork_cb);

void subprocess_close(subprocess_t *subprocess);

/*
 * Return the captured subprocess output or error stream and move its
 * ownership to the caller.
 */
bytestream_1 subprocess_release_stdout(subprocess_t *subprocess);
bytestream_1 subprocess_release_stderr(subprocess_t *subprocess);
pid_t subprocess_get_pid(subprocess_t *subprocess);
/*
 * A negative exit status value -N indicates that the subprocess
 * terminated due to receipt of signal N.
 */
bool subprocess_wait(subprocess_t *subprocess, int *exit_status);

#ifdef __cplusplus
}
#endif
