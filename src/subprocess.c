#include "subprocess.h"

#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fsdyn/integer.h>
#include <fstrace.h>
#include <unixkit/unixkit.h>

#include "drystream.h"
#include "pipestream.h"

struct subprocess {
    async_t *async;
    uint64_t uid;
    pid_t pid;
    bytestream_1 stdout_stream;
    bytestream_1 stderr_stream;
};

FSTRACE_DECL(ASYNC_SUBPROCESS_CREATE, "UID=%64u PTR=%p ASYNC=%p PID=%64u");
FSTRACE_DECL(ASYNC_SUBPROCESS_CREATE_SOCKETPAIR_FAIL, "ERRNO=%e");
FSTRACE_DECL(ASYNC_SUBPROCESS_CREATE_FORK_FAIL, "ERRNO=%e");

subprocess_t *open_subprocess(async_t *async, list_t *keep_fds,
                              bool capture_stdout, bool capture_stderr,
                              action_1 post_fork_cb)
{
    int stdout_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };

    if (capture_stdout) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, stdout_pipe) < 0) {
            FSTRACE(ASYNC_SUBPROCESS_CREATE_SOCKETPAIR_FAIL);
            goto fail;
        }
        list_append(keep_fds, as_integer(stdout_pipe[1]));
    }

    if (capture_stderr) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, stderr_pipe) < 0) {
            FSTRACE(ASYNC_SUBPROCESS_CREATE_SOCKETPAIR_FAIL);
            goto fail;
        }
        list_append(keep_fds, as_integer(stderr_pipe[1]));
    }

    list_append(keep_fds, as_integer(1));
    list_append(keep_fds, as_integer(2));
    pid_t pid = unixkit_fork(keep_fds);
    if (pid == -1) {
        FSTRACE(ASYNC_SUBPROCESS_CREATE_FORK_FAIL);
        goto fail;
    }

    if (pid == 0) {
        if (capture_stdout) {
            dup2(stdout_pipe[1], 1);
            close(stdout_pipe[1]);
        }
        if (capture_stderr) {
            dup2(stderr_pipe[1], 2);
            close(stderr_pipe[1]);
        }
        action_1_perf(post_fork_cb);
        _exit(1);
    }

    subprocess_t *subprocess = fsalloc(sizeof *subprocess);
    subprocess->async = async;
    subprocess->uid = fstrace_get_unique_id();
    subprocess->pid = pid;
    subprocess->stdout_stream = drystream;
    subprocess->stderr_stream = drystream;

    if (capture_stdout) {
        close(stdout_pipe[1]);
        subprocess->stdout_stream =
            pipestream_as_bytestream_1(open_pipestream(async, stdout_pipe[0]));
    }

    if (capture_stderr) {
        close(stderr_pipe[1]);
        subprocess->stderr_stream =
            pipestream_as_bytestream_1(open_pipestream(async, stderr_pipe[0]));
    }

    FSTRACE(ASYNC_SUBPROCESS_CREATE, subprocess->uid, subprocess, async,
            (uint64_t) pid);
    return subprocess;

fail:
    if (stdout_pipe[0] >= 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
    }
    if (stderr_pipe[0] >= 0) {
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
    }
    return NULL;
}

FSTRACE_DECL(ASYNC_SUBPROCESS_CLOSE, "UID=%64u");

void subprocess_close(subprocess_t *subprocess)
{
    FSTRACE(ASYNC_SUBPROCESS_CLOSE, subprocess->uid);
    bytestream_1_close(subprocess->stdout_stream);
    bytestream_1_close(subprocess->stderr_stream);
    fsfree(subprocess);
}

FSTRACE_DECL(ASYNC_SUBPROCESS_RELEASE_STDOUT, "UID=%64u");

bytestream_1 subprocess_release_stdout(subprocess_t *subprocess)
{
    FSTRACE(ASYNC_SUBPROCESS_RELEASE_STDOUT, subprocess->uid);
    bytestream_1 stream = subprocess->stdout_stream;
    subprocess->stdout_stream = drystream;
    return stream;
}

FSTRACE_DECL(ASYNC_SUBPROCESS_RELEASE_STDERR, "UID=%64u");

bytestream_1 subprocess_release_stderr(subprocess_t *subprocess)
{
    FSTRACE(ASYNC_SUBPROCESS_RELEASE_STDERR, subprocess->uid);
    bytestream_1 stream = subprocess->stderr_stream;
    subprocess->stderr_stream = drystream;
    return stream;
}

pid_t subprocess_get_pid(subprocess_t *subprocess)
{
    return subprocess->pid;
}

FSTRACE_DECL(ASYNC_SUBPROCESS_WAIT_START, "UID=%64u");
FSTRACE_DECL(ASYNC_SUBPROCESS_WAIT_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_SUBPROCESS_WAIT, "UID=%64u EXIT-STATUS=%d");

bool subprocess_wait(subprocess_t *subprocess, int *exit_status)
{
    FSTRACE(ASYNC_SUBPROCESS_WAIT_START, subprocess->uid);
    int status;
    if (waitpid(subprocess->pid, &status, 0) < 0) {
        FSTRACE(ASYNC_SUBPROCESS_WAIT_FAIL, subprocess->uid);
        return false;
    }
    if (WIFEXITED(status))
        *exit_status = WEXITSTATUS(status);
    else {
        assert(WIFSIGNALED(status));
        *exit_status = -WTERMSIG(status);
    }
    FSTRACE(ASYNC_SUBPROCESS_WAIT, subprocess->uid, *exit_status);
    return true;
}
