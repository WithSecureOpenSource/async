#include "alock.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>

#include <fsdyn/integer.h>
#include <fstrace.h>

#include "jsonthreader.h"

typedef struct {
    const char *path;
    int lock_fd;
} alock_ctx_t;

typedef enum {
    ALOCK_IDLE,
    ALOCK_AWAITING_RESPONSE,
    ALOCK_ZOMBIE,
} alock_state_t;

struct alock {
    async_t *async;
    uint64_t uid;
    jsonthreader_t *threader;
    alock_state_t state;
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

static bool lock(alock_ctx_t *ctx, int op)
{
    FSTRACE(ASYNC_ALOCK_SERVER_LOCK, trace_flock_op, &op);
    if (ctx->lock_fd < 0) {
        ctx->lock_fd = open(ctx->path, O_RDONLY);
        if (ctx->lock_fd < 0) {
            FSTRACE(ASYNC_ALOCK_SERVER_LOCK_OPEN_FAIL);
            return false;
        }
    }
    if (flock(ctx->lock_fd, op) < 0) {
        FSTRACE(ASYNC_ALOCK_SERVER_LOCK_FLOCK_FAIL);
        return false;
    }
    return true;
}

static json_thing_t *handle_request(void *obj, json_thing_t *request)
{
    alock_ctx_t *ctx = obj;
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
    if (lock(ctx, op)) {
        bool locked = op == LOCK_EX;
        json_add_to_object(response, "status", json_make_string("success"));
        json_add_to_object(response, "locked", json_make_boolean(locked));
    } else {
        json_add_to_object(response, "status", json_make_string("failure"));
        json_add_to_object(response, "error", json_make_unsigned(errno));
    }
    return response;
}

FSTRACE_DECL(ASYNC_ALOCK_CREATE, "UID=%64u ASYNC=%p PATH=%s THREADER=%p");
FSTRACE_DECL(ASYNC_ALOCK_CREATE_JSONTHREADER_FAIL, "ERROR=%e");

alock_t *make_alock(async_t *async, const char *path, action_1 post_fork_cb)
{
    alock_ctx_t *ctx = fsalloc(sizeof *ctx);
    ctx->lock_fd = -1;
    ctx->path = path;
    list_t *keep_fds = make_list();
    list_append(keep_fds, as_integer(0));
    list_append(keep_fds, as_integer(1));
    list_append(keep_fds, as_integer(2));
    jsonthreader_t *threader = make_jsonthreader(async, keep_fds, post_fork_cb,
                                                 handle_request, ctx, 8192, 1);
    fsfree(ctx);
    if (!threader) {
        FSTRACE(ASYNC_ALOCK_CREATE_JSONTHREADER_FAIL);
        return NULL;
    }
    alock_t *alock = fsalloc(sizeof *alock);
    alock->async = async;
    alock->uid = fstrace_get_unique_id();
    alock->threader = threader;
    alock->state = ALOCK_IDLE;
    FSTRACE(ASYNC_ALOCK_CREATE, alock->uid, async, path, threader);
    return alock;
}

FSTRACE_DECL(ASYNC_ALOCK_DESTROY, "UID=%64u");

void destroy_alock(alock_t *alock)
{
    FSTRACE(ASYNC_ALOCK_DESTROY, alock->uid);
    jsonthreader_terminate(alock->threader);
    destroy_jsonthreader(alock->threader);
    async_wound(alock->async, alock);
    alock->state = ALOCK_ZOMBIE;
}

FSTRACE_DECL(ASYNC_ALOCK_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void alock_register_callback(alock_t *alock, action_1 action)
{
    FSTRACE(ASYNC_ALOCK_REGISTER, alock->uid, action.obj, action.act);
    jsonthreader_register_callback(alock->threader, action);
}

FSTRACE_DECL(ASYNC_ALOCK_UNREGISTER, "UID=%64u");

void alock_unregister_callback(alock_t *alock)
{
    FSTRACE(ASYNC_ALOCK_UNREGISTER, alock->uid);
    jsonthreader_register_callback(alock->threader, NULL_ACTION_1);
}

static bool send_request(alock_t *alock, const char *type)
{
    if (alock->state != ALOCK_IDLE) {
        errno = EAGAIN;
        return false;
    }
    json_thing_t *request = json_make_object();
    json_add_to_object(request, "type", json_make_string(type));
    jsonthreader_send(alock->threader, request);
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

static bool parse_response(json_thing_t *thing, unsigned long long *error,
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
    json_thing_t *thing = jsonthreader_receive(alock->threader);
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
