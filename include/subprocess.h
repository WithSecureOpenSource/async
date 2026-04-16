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
 * The post-fork callback is invoked in the child process after fork,
 * concurrently with the parent. It is typically
 * used to call exec(3). If the callback returns without calling exec,
 * the child calls _exit(1). The child must not use the given async
 * object (behaviour is undefined) or call exit(3) (behaviour is
 * undefined).
 *
 * All file descriptors except those listed in keep_fds are closed in
 * the child process. This function takes ownership of keep_fds and
 * appends the stdout/stderr pipe endpoints to it internally, so the
 * caller must not include those.
 */
subprocess_t *open_subprocess(async_t *async, list_t *keep_fds,
                              bool capture_stdout, bool capture_stderr,
                              action_1 post_fork_cb);

/*
 * Close the subprocess object and any captured streams that have not
 * been released. Does not kill or wait for the child process; call
 * subprocess_wait first to avoid leaving a zombie.
 */
void subprocess_close(subprocess_t *subprocess);

/*
 * Transfer ownership of the captured stdout (or stderr) stream to the
 * caller. After release, the stream is the caller's responsibility to
 * close, and subprocess_close will no longer affect it. Returns a dry
 * (empty) stream if capture was not requested or the stream was
 * already released.
 */
bytestream_1 subprocess_release_stdout(subprocess_t *subprocess);
bytestream_1 subprocess_release_stderr(subprocess_t *subprocess);
pid_t subprocess_get_pid(subprocess_t *subprocess);

/*
 * Wait for the child process to terminate and return its exit status.
 * A negative value -N indicates termination by signal N.
 *
 * The intended use is to call this after EOF has been read from the
 * captured stdout/stderr streams, by which time the well-behaved
 * child process has exited or is about to.
 */
bool subprocess_wait(subprocess_t *subprocess, int *exit_status);

#ifdef __cplusplus
}
#endif
