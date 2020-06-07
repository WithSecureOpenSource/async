#include "alock.h"

#include "json_connection.h"

#include <fsdyn/integer.h>
#include <fstrace.h>
#include <unixkit/unixkit.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    async_t *async;
    json_conn_t *conn;
    const char *path;
    int lock_fd;
} alock_server_t;

typedef enum {
    ALOCK_IDLE,
    ALOCK_AWAITING_RESPONSE,
    ALOCK_ZOMBIE,
} alock_state_t;

struct alock {
    async_t *async;
    uint64_t uid;
    pid_t child_pid;
    alock_state_t state;
    json_conn_t *conn;
};

static const char *trace_flock_op(int *op)
{
    switch (*op) {
        case LOCK_EX:
            return "LOCK_EX";
        case LOCK_UN:
            return "LOCK_UN";
        default:
            return fstrace_signed_repr(*op);
    }
}

FSTRACE_DECL(ASYNC_ALOCK_SERVER_LOCK, "PID=%P OP=%I");
FSTRACE_DECL(ASYNC_ALOCK_SERVER_LOCK_OPEN_FAIL, "PID=%P ERROR=%e");
FSTRACE_DECL(ASYNC_ALOCK_SERVER_LOCK_FLOCK_FAIL, "PID=%P ERROR=%e");

static bool alock_server_lock(alock_server_t *server, int op)
{
    FSTRACE(ASYNC_ALOCK_SERVER_LOCK, trace_flock_op, &op);
    if (server->lock_fd < 0) {
        server->lock_fd = open(server->path, O_RDONLY);
        if (server->lock_fd < 0) {
            FSTRACE(ASYNC_ALOCK_SERVER_LOCK_OPEN_FAIL);
            return false;
        }
    }
    if (flock(server->lock_fd, op) < 0) {
        FSTRACE(ASYNC_ALOCK_SERVER_LOCK_FLOCK_FAIL);
        return false;
    }
    return true;
}

FSTRACE_DECL(ASYNC_ALOCK_SERVER_PROBE_RECEIVE_EOF, "PID=%P");
FSTRACE_DECL(ASYNC_ALOCK_SERVER_PROBE_RECEIVE_FAIL, "PID=%P ERROR=%e");

static void alock_server_probe(alock_server_t *server)
{
    json_thing_t *request = json_conn_receive(server->conn);
    if (!request) {
        if (errno == EAGAIN)
            return;
        if (errno == 0)
            FSTRACE(ASYNC_ALOCK_SERVER_PROBE_RECEIVE_EOF);
        else
            FSTRACE(ASYNC_ALOCK_SERVER_PROBE_RECEIVE_FAIL);
        async_quit_loop(server->async);
        return;
    }

    const char *type;
    json_object_get_string(request, "type", &type);
    int op;
    if (!strcmp(type, "lock"))
        op = LOCK_EX;
    else if (!strcmp(type, "unlock"))
        op = LOCK_UN;
    else
        assert(false);

    json_thing_t *response = json_make_object();
    if (alock_server_lock(server, op)) {
        bool locked = op == LOCK_EX;
        json_add_to_object(response, "status", json_make_string("success"));
        json_add_to_object(response, "locked", json_make_boolean(locked));
    } else {
        json_add_to_object(response, "status", json_make_string("failure"));
        json_add_to_object(response, "error", json_make_unsigned(errno));
    }
    json_conn_send(server->conn, response);
    json_destroy_thing(response);

    json_destroy_thing(request);
    action_1 probe_cb = { server, (act_1) alock_server_probe };
    async_execute(server->async, probe_cb);
}

FSTRACE_DECL(ASYNC_ALOCK_ASYNC_FAIL, "PID=%P ERROR=%e");

static void run_alock_server(int fd, const char *path)
{
    async_t *async = make_async();
    if (!async) {
        FSTRACE(ASYNC_ALOCK_ASYNC_FAIL);
        return;
    }
    tcp_conn_t *tcp_conn = tcp_adopt_connection(async, fd);
    json_conn_t *conn = open_json_conn(async, tcp_conn, 8192);
    alock_server_t server = {
        .async = async,
        .conn = conn,
        .path = path,
        .lock_fd = -1,
    };
    action_1 probe_cb = { &server, (act_1) alock_server_probe };
    json_conn_register_callback(server.conn, probe_cb);
    async_execute(async, probe_cb);
    while (async_loop(async) < 0)
        if (errno != EINTR)
            break;
}

FSTRACE_DECL(ASYNC_ALOCK_CREATE, "UID=%64u ASYNC=%p PATH=%s CHILD-PID=%u");
FSTRACE_DECL(ASYNC_ALOCK_CREATE_SOCKETPAIR_FAIL, "ERROR=%e");

alock_t *make_alock(async_t *async, const char *path, action_1 post_fork_cb)
{
    int pairfd[2];
    if (socketpair(PF_UNIX, SOCK_STREAM, 0, pairfd) < 0) {
        FSTRACE(ASYNC_ALOCK_CREATE_SOCKETPAIR_FAIL);
        return NULL;
    }

    list_t *keep_fds = make_list();
    list_append(keep_fds, as_integer(0));
    list_append(keep_fds, as_integer(1));
    list_append(keep_fds, as_integer(2));
    list_append(keep_fds, as_integer(pairfd[0]));
    pid_t child_pid = unixkit_fork(keep_fds);
    if (child_pid < 0) {
        close(pairfd[0]);
        close(pairfd[1]);
        return NULL;
    }
    if (child_pid == 0) {
        action_1_perf(post_fork_cb);
        run_alock_server(pairfd[0], path);
        _exit(0);
    }
    close(pairfd[0]);
    alock_t *alock = fsalloc(sizeof *alock);
    alock->async = async;
    alock->uid = fstrace_get_unique_id();
    alock->child_pid = child_pid;
    alock->state = ALOCK_IDLE;
    tcp_conn_t *tcp_conn = tcp_adopt_connection(async, pairfd[1]);
    alock->conn = open_json_conn(async, tcp_conn, 8192);
    FSTRACE(ASYNC_ALOCK_CREATE, alock->uid, async, path, child_pid);
    return alock;
}

FSTRACE_DECL(ASYNC_ALOCK_DESTROY, "UID=%64u");
FSTRACE_DECL(ASYNC_ALOCK_DESTROY_WAITPID_FAIL, "UID=%64u, ERROR=%e");

void destroy_alock(alock_t *alock)
{
    FSTRACE(ASYNC_ALOCK_DESTROY, alock->uid);
    json_conn_close(alock->conn);
    kill(alock->child_pid, SIGTERM);
    while (waitpid(alock->child_pid, NULL, 0) < 0) {
        if (errno != EINTR) {
            FSTRACE(ASYNC_ALOCK_DESTROY_WAITPID_FAIL, alock->uid);
            break;
        }
    }
    async_wound(alock->async, alock);
    alock->state = ALOCK_ZOMBIE;
}

FSTRACE_DECL(ASYNC_ALOCK_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void alock_register_callback(alock_t *alock, action_1 action)
{
    FSTRACE(ASYNC_ALOCK_REGISTER, alock->uid, action.obj, action.act);
    json_conn_register_callback(alock->conn, action);
}

FSTRACE_DECL(ASYNC_ALOCK_UNREGISTER, "UID=%64u");

void alock_unregister_callback(alock_t *alock)
{
    FSTRACE(ASYNC_ALOCK_UNREGISTER, alock->uid);
    json_conn_register_callback(alock->conn, NULL_ACTION_1);
}

static bool send_request(alock_t *alock, const char *type)
{
    if (alock->state != ALOCK_IDLE) {
        errno = EAGAIN;
        return false;
    }
    json_thing_t *request = json_make_object();
    json_add_to_object(request, "type", json_make_string(type));
    json_conn_send(alock->conn, request);
    json_destroy_thing(request);
    alock->state = ALOCK_AWAITING_RESPONSE;
    return true;
}

FSTRACE_DECL(ASYNC_ALOCK_LOCK, "UID=%64u");

bool alock_lock(alock_t *alock)
{
    FSTRACE(ASYNC_ALOCK_LOCK, alock->uid);
    return send_request(alock, "lock");
}

FSTRACE_DECL(ASYNC_ALOCK_UNLOCK, "UID=%64u");

bool alock_unlock(alock_t *alock)
{
    FSTRACE(ASYNC_ALOCK_UNLOCK, alock->uid);
    return send_request(alock, "unlock");
}

static bool parse_response(json_thing_t *thing,
                           unsigned long long *error,
                           bool *locked)
{
    const char *status;
    if (!json_object_get_string(thing, "status", &status))
        return false;
    *error = 0;
    if (!strcmp(status, "success"))
        return json_object_get_boolean(thing, "locked", locked);
    else if (!strcmp(status, "failure"))
        return json_object_get_unsigned(thing, "error", error);
    else
        return false;
}

FSTRACE_DECL(ASYNC_ALOCK_CHECK_RECEIVE_EOF, "UID=%64u");
FSTRACE_DECL(ASYNC_ALOCK_CHECK_RECEIVE_SPURIOUS, "UID=%64u");
FSTRACE_DECL(ASYNC_ALOCK_CHECK_RECEIVE_FAIL, "UID=%64u ERROR=%e");
FSTRACE_DECL(ASYNC_ALOCK_CHECK_FAIL, "UID=%64u ERROR=%e");
FSTRACE_DECL(ASYNC_ALOCK_CHECK, "UID=%64u LOCKED=%b");

bool alock_check(alock_t *alock, bool *locked)
{
    if (alock->state == ALOCK_ZOMBIE) {
        errno = EINVAL;
        return false;
    }
    json_thing_t *thing = json_conn_receive(alock->conn);
    if (!thing) {
        switch (errno) {
            case 0:
                FSTRACE(ASYNC_ALOCK_CHECK_RECEIVE_EOF, alock->uid);
                errno = EPROTO;
                break;
            case EAGAIN:
                FSTRACE(ASYNC_ALOCK_CHECK_RECEIVE_SPURIOUS, alock->uid);
                break;
            default:
                FSTRACE(ASYNC_ALOCK_CHECK_RECEIVE_FAIL, alock->uid);
        }
        return false;
    }

    errno = 0;
    switch (alock->state) {
        case ALOCK_IDLE:
            errno = EPROTO;
            break;
        case ALOCK_AWAITING_RESPONSE:
            alock->state = ALOCK_IDLE;
            unsigned long long error;
            if (!parse_response(thing, &error, locked))
                errno = EPROTO;
            else if (error)
                errno = error;
            break;
        default:
            assert(false);
    }
    json_destroy_thing(thing);
    if (errno) {
        FSTRACE(ASYNC_ALOCK_CHECK_FAIL, alock->uid);
        return false;
    }
    FSTRACE(ASYNC_ALOCK_CHECK, alock->uid, *locked);
    return true;
}
