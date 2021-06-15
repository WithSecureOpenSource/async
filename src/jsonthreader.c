#include "jsonthreader.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>

#include <fsdyn/integer.h>
#include <fstrace.h>
#include <unixkit/unixkit.h>

#include "json_connection.h"
#include "tcp_connection.h"

struct jsonthreader {
    uint64_t uid;
    json_conn_t *conn;
    pid_t child_pid;
};

typedef struct {
    async_t *async;
    json_conn_t *conn;
    json_thing_t *(*handler)(void *, json_thing_t *);
    void *obj;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned ready;
    unsigned quota;
} jsonthreader_bh_t;

static void lock(void *obj)
{
    jsonthreader_bh_t *bh = obj;
    pthread_mutex_lock(&bh->lock);
}

static void unlock(void *obj)
{
    jsonthreader_bh_t *bh = obj;
    pthread_mutex_unlock(&bh->lock);
}

static void notify(jsonthreader_bh_t *bh)
{
    pthread_cond_signal(&bh->cond);
}

static void await_notification(jsonthreader_bh_t *bh)
{
    pthread_cond_wait(&bh->cond, &bh->lock);
}

static void serve(void *obj);

FSTRACE_DECL(ASYNC_JSONTHREADER_MT_PROBE_NOTIFY, "PID=%P TID=%T");
FSTRACE_DECL(ASYNC_JSONTHREADER_MT_PROBE_BUSY, "PID=%P TID=%T");
FSTRACE_DECL(ASYNC_JSONTHREADER_MT_PROBE_PTHREAD_CREATE_FAIL,
             "PID=%P TID=%T ERRNO=%E");
FSTRACE_DECL(ASYNC_JSONTHREADER_MT_PROBE_PTHREAD_CREATE,
             "PID=%P TID=%T READY=%u QUOTA=%u");

static void mt_probe(jsonthreader_bh_t *bh)
{
    if (bh->ready) {
        notify(bh);
        FSTRACE(ASYNC_JSONTHREADER_MT_PROBE_NOTIFY);
        return;
    }
    if (!bh->quota) {
        FSTRACE(ASYNC_JSONTHREADER_MT_PROBE_BUSY);
        return;
    }
    pthread_t t;
    int err = pthread_create(&t, NULL, (void *) serve, bh);
    if (err)
        FSTRACE(ASYNC_JSONTHREADER_MT_PROBE_PTHREAD_CREATE_FAIL, err);
    else {
        bh->ready++;
        bh->quota--;
        FSTRACE(ASYNC_JSONTHREADER_MT_PROBE_PTHREAD_CREATE, bh->ready,
                bh->quota);
    }
}

FSTRACE_DECL(ASYNC_JSONTHREADER_SERVE, "PID=%P TID=%T REQ=%I");
FSTRACE_DECL(ASYNC_JSONTHREADER_SERVE_LOCK, "PID=%P TID=%T");
FSTRACE_DECL(ASYNC_JSONTHREADER_SERVE_LOCKED, "PID=%P TID=%T");
FSTRACE_DECL(ASYNC_JSONTHREADER_SERVE_AWAIT, "PID=%P TID=%T");
FSTRACE_DECL(ASYNC_JSONTHREADER_SERVE_NOTIFIED, "PID=%P TID=%T");
FSTRACE_DECL(ASYNC_JSONTHREADER_SERVE_FAIL, "PID=%P TID=%T ERRNO=%e");

static void serve(void *obj)
{
    jsonthreader_bh_t *bh = obj;
    FSTRACE(ASYNC_JSONTHREADER_SERVE_LOCK);
    lock(bh);
    FSTRACE(ASYNC_JSONTHREADER_SERVE_LOCKED);
    for (;;) {
        json_thing_t *request = json_conn_receive(bh->conn);
        if (request) {
            bh->ready--;
            mt_probe(bh);
            unlock(bh);
            json_thing_t *response = bh->handler(bh->obj, request);
            FSTRACE(ASYNC_JSONTHREADER_SERVE, json_trace, request);
            lock(bh);
            json_destroy_thing(request);
            if (response) {
                json_conn_send(bh->conn, response);
                json_destroy_thing(response);
            }
            bh->ready++;
        } else if (errno == EAGAIN) {
            FSTRACE(ASYNC_JSONTHREADER_SERVE_AWAIT);
            await_notification(bh);
            FSTRACE(ASYNC_JSONTHREADER_SERVE_NOTIFIED);
        } else {
            FSTRACE(ASYNC_JSONTHREADER_SERVE_FAIL);
            async_quit_loop(bh->async);
            unlock(bh);
            return;
        }
    }
}

FSTRACE_DECL(ASYNC_JSONTHREADER_PROBE, "PID=%P REQ=%I");
FSTRACE_DECL(ASYNC_JSONTHREADER_PROBE_SPURIOUS, "PID=%P");
FSTRACE_DECL(ASYNC_JSONTHREADER_PROBE_FAIL, "PID=%P ERRNO=%e");

static void probe(jsonthreader_bh_t *bh)
{
    json_thing_t *request = json_conn_receive(bh->conn);
    if (!request) {
        if (errno == EAGAIN) {
            FSTRACE(ASYNC_JSONTHREADER_PROBE_SPURIOUS);
            return;
        }
        FSTRACE(ASYNC_JSONTHREADER_PROBE_FAIL);
        async_quit_loop(bh->async);
        return;
    }

    json_thing_t *response = bh->handler(bh->obj, request);
    FSTRACE(ASYNC_JSONTHREADER_PROBE, json_trace, request);
    json_destroy_thing(request);
    if (response) {
        json_conn_send(bh->conn, response);
        json_destroy_thing(response);
    }

    action_1 probe_cb = { bh, (act_1) probe };
    async_execute(bh->async, probe_cb);
}

FSTRACE_DECL(ASYNC_JSONTHREADER_RUN, "PID=%P");
FSTRACE_DECL(ASYNC_JSONTHREADER_RUN_ASYNC_FAIL, "PID=%P ERRNO=%e");

static void run(int fd, json_thing_t *(*handler)(void *, json_thing_t *),
                void *obj, size_t max_frame_size, unsigned max_parallel)
{
    FSTRACE(ASYNC_JSONTHREADER_RUN);
    assert(max_parallel >= 1);
    async_t *async = make_async();
    if (!async) {
        FSTRACE(ASYNC_JSONTHREADER_RUN_ASYNC_FAIL);
        return;
    }
    tcp_conn_t *tcp_conn = tcp_adopt_connection(async, fd);
    json_conn_t *conn = open_json_conn(async, tcp_conn, max_frame_size);
    jsonthreader_bh_t bh = {
        .async = async,
        .conn = conn,
        .handler = handler,
        .obj = obj,
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .ready = 0,
        .quota = max_parallel,
    };
    if (max_parallel > 1) {
        action_1 probe_cb = { &bh, (act_1) mt_probe };
        json_conn_register_callback(bh.conn, probe_cb);
        lock(&bh);
        mt_probe(&bh);
        while (async_loop_protected(async, lock, unlock, &bh) < 0)
            if (errno != EINTR)
                break;
    } else {
        action_1 probe_cb = { &bh, (act_1) probe };
        json_conn_register_callback(bh.conn, probe_cb);
        async_execute(async, probe_cb);
        while (async_loop(async) < 0)
            if (errno != EINTR)
                break;
    }
}

FSTRACE_DECL(ASYNC_JSONTHREADER_CREATE,
             "UID=%64u PTR=%p CHILD-PID=%u JSON-CONN=%p");

jsonthreader_t *make_jsonthreader(
    async_t *async, list_t *keep_fds, action_1 post_fork_cb,
    json_thing_t *(*handler)(void *, json_thing_t *), void *obj,
    size_t max_frame_size, unsigned max_parallel)
{
    int pairfd[2];
    if (!unixkit_socketpair(AF_UNIX, SOCK_STREAM, 0, pairfd))
        return NULL;
    list_append(keep_fds, as_integer(pairfd[0]));
    pid_t child_pid = unixkit_fork(keep_fds);
    if (child_pid == -1) {
        close(pairfd[0]);
        close(pairfd[1]);
        return NULL;
    }
    if (child_pid == 0) {
        action_1_perf(post_fork_cb);
        run(pairfd[0], handler, obj, max_frame_size, max_parallel);
        _exit(0);
    }
    close(pairfd[0]);

    jsonthreader_t *threader = fsalloc(sizeof *threader);
    threader->uid = fstrace_get_unique_id();
    threader->child_pid = child_pid;
    tcp_conn_t *tcp_conn = tcp_adopt_connection(async, pairfd[1]);
    threader->conn = open_json_conn(async, tcp_conn, max_frame_size);
    FSTRACE(ASYNC_JSONTHREADER_CREATE, threader->uid, threader, child_pid,
            threader->conn);
    return threader;
}

FSTRACE_DECL(ASYNC_JSONTHREADER_DESTROY, "UID=%64u");

void destroy_jsonthreader(jsonthreader_t *threader)
{
    FSTRACE(ASYNC_JSONTHREADER_DESTROY, threader->uid);
    json_conn_close(threader->conn);
    fsfree(threader);
}

FSTRACE_DECL(ASYNC_JSONTHREADER_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void jsonthreader_register_callback(jsonthreader_t *threader, action_1 action)
{
    FSTRACE(ASYNC_JSONTHREADER_REGISTER, threader->uid, action.obj, action.act);
    json_conn_register_callback(threader->conn, action);
}

FSTRACE_DECL(ASYNC_JSONTHREADER_UNREGISTER, "UID=%64u");

void jsonthreader_unregister_callback(jsonthreader_t *threader)
{
    FSTRACE(ASYNC_JSONTHREADER_UNREGISTER, threader->uid);
    json_conn_unregister_callback(threader->conn);
}

FSTRACE_DECL(ASYNC_JSONTHREADER_SEND, "UID=%64u REQ=%I");

void jsonthreader_send(jsonthreader_t *threader, json_thing_t *thing)
{
    FSTRACE(ASYNC_JSONTHREADER_SEND, threader->uid, json_trace, thing);
    json_conn_send(threader->conn, thing);
}

FSTRACE_DECL(ASYNC_JSONTHREADER_RECEIVE, "UID=%64u ERRNO=%e");

json_thing_t *jsonthreader_receive(jsonthreader_t *threader)
{
    json_thing_t *thing = json_conn_receive(threader->conn);
    FSTRACE(ASYNC_JSONTHREADER_RECEIVE, threader->uid);
    return thing;
}

FSTRACE_DECL(ASYNC_JSONTHREADER_TERMINATE, "UID=%64u");
FSTRACE_DECL(ASYNC_JSONTHREADER_TERMINATE_WAITPID_FAIL, "UID=%64u ERRNO=%e");

void jsonthreader_terminate(jsonthreader_t *threader)
{
    FSTRACE(ASYNC_JSONTHREADER_TERMINATE, threader->uid);
    pid_t pid = threader->child_pid;
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0) {
        if (errno != EINTR) {
            FSTRACE(ASYNC_JSONTHREADER_TERMINATE_WAITPID_FAIL, threader->uid);
            break;
        }
    }
}
