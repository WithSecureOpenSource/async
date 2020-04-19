#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <assert.h>
#include <unixkit/unixkit.h>
#include <fsdyn/fsalloc.h>
#include <fsdyn/charstr.h>
#include <fsdyn/base64.h>
#include <fsdyn/list.h>
#include <fsdyn/hashtable.h>
#include <fstrace.h>
#include "json_connection.h"
#include "fsadns.h"
#include "async_version.h"

struct fsadns {
    async_t *async;
    uint64_t uid;
    int error;                  /* if non-0, the error is fatal */
    unsigned max_parallel;
    pid_t child;
    json_conn_t *conn;
    hash_table_t *query_map;
    list_t *queries;
};

typedef enum {
    QUERY_REQUESTED_ADDRESS,
    QUERY_REQUESTED_NAME,
    QUERY_CANCELED,
    QUERY_REPLIED_ADDRESS,
    QUERY_REPLIED_NAME,
    QUERY_CONSUMED,
    QUERY_ERRORED,
    QUERY_ZOMBIE,
} query_state_t;

struct fsadns_query {
    fsadns_t *dns;
    action_1 probe;
    uint64_t uid;
    query_state_t state;
    union {
        struct {                /* QUERY_REPLIED_ADDRESS only */
            struct addrinfo *info;
        } address;
        struct {                /* QUERY_REPLIED_NAME only */
            char *host, *serv;
        } name;
        struct {                /* QUERY_ERRORED only */
            int err, err_no;
        } error;
    };
    list_elem_t *loc;           /* in dns->queries */
};

static uint64_t query_hash(const void *key)
{
    return *(const uint64_t *) key;
}

static int query_cmp(const void *key1, const void *key2)
{
    uint64_t uid1 = *(const uint64_t *) key1;
    uint64_t uid2 = *(const uint64_t *) key2;
    if (uid1 < uid2)
        return -1;
    if (uid1 > uid2)
        return 1;
    return 0;
}

typedef struct {
    fsadns_t *dns;
    async_t *async;             /* for the slave process */
    json_conn_t *conn;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned ready, quota_remaining; /* sync'ed with acct_lock */
} shared_t;

static void construct_resolve_address_fail(json_thing_t *resp,
                                           int err, int syserr)
{
    json_thing_t *fields = json_make_object();
    json_add_to_object(resp, "resolve_address_fail", fields);
    json_add_to_object(fields, "error", json_make_integer(err));
    if (err == EAI_SYSTEM)
        json_add_to_object(fields, "errno", json_make_integer(syserr));
}

static json_thing_t *construct_addrinfo(const struct addrinfo *ap)
{
    json_thing_t *info = json_make_object();
    json_add_to_object(info, "flags", json_make_integer(ap->ai_flags));
    json_add_to_object(info, "family", json_make_integer(ap->ai_family));
    json_add_to_object(info, "socketype", json_make_integer(ap->ai_socktype));
    json_add_to_object(info, "protocol", json_make_integer(ap->ai_protocol));
    char *base64_encoded = base64_encode_simple(ap->ai_addr, ap->ai_addrlen);
    json_add_to_object(info, "addr", json_adopt_string(base64_encoded));
    if (ap->ai_flags & AI_CANONNAME) {
        char *url_encoded = charstr_url_encode(ap->ai_canonname);
        json_add_to_object(info, "canonname",
                           json_adopt_string(url_encoded));
    }
    return info;
}

static void construct_resolve_address_resp(json_thing_t *resp,
                                           const struct addrinfo *res)
{
    json_thing_t *fields = json_make_object();
    json_add_to_object(resp, "resolve_address_resp", fields);
    json_thing_t *responses = json_make_array();
    json_add_to_object(fields, "responses", responses);
    const struct addrinfo *ap;
    for (ap = res; ap; ap = ap->ai_next)
        json_add_to_array(responses, construct_addrinfo(ap));
}

static void deconstruct_addrinfo(struct addrinfo *ap, json_thing_t *info)
{
    long long n;
    if (json_object_get_integer(info, "flags", &n))
        ap->ai_flags = n;
    if (json_object_get_integer(info, "family", &n))
        ap->ai_family = n;
    if (json_object_get_integer(info, "socktype", &n))
        ap->ai_socktype = n;
    if (json_object_get_integer(info, "protocol", &n))
        ap->ai_protocol = n;
    const char *s;
    if (json_object_get_string(info, "addr", &s)) {
        size_t len;
        ap->ai_addr = base64_decode_simple(s, &len);
        assert(ap->ai_addr);
        ap->ai_addrlen = len;
    }
    if (json_object_get_string(info, "canonname", &s))
        ap->ai_canonname = charstr_url_decode(s, true, NULL);
}

static const char *trace_getaddrinfo_error(void *perror)
{
    switch (*(int *) perror) {
        case EAI_AGAIN:
            return "EAI_AGAIN";
        case EAI_BADFLAGS:
            return "EAI_BADFLAGS";
        case EAI_FAIL:
            return "EAI_FAIL";
        case EAI_FAMILY:
            return "EAI_FAMILY";
        case EAI_MEMORY:
            return "EAI_MEMORY";
        case EAI_OVERFLOW:
            return "EAI_MEMORY";
        case EAI_NONAME:
            return "EAI_NONAME";
        case EAI_SERVICE:
            return "EAI_SERVICE";
        case EAI_SOCKTYPE:
            return "EAI_SOCKTYPE";
        case EAI_SYSTEM:
            return "EAI_SYSTEM";
        default:
            return fstrace_signed_repr(*(int *) perror);
    }
}

FSTRACE_DECL(FSADNS_SERVE_GETADDRINFO_START, "UID=%64u PID=%P TID=%T");
FSTRACE_DECL(FSADNS_SERVE_GETADDRINFO, "UID=%64u PID=%P TID=%T");
FSTRACE_DECL(FSADNS_SERVE_GETADDRINFO_FAIL,
             "UID=%64u PID=%P TID=%T ERR=%I ERRNO=%e");

static json_thing_t *resolve_address(shared_t *shared, json_thing_t *reqid,
                                     json_thing_t *fields)
{
    struct addrinfo hints = { .ai_flags = 0 }, *phints, *res;
    json_thing_t *hint_fields;
    if (json_object_get_object(fields, "hints", &hint_fields)) {
        phints = &hints;
        deconstruct_addrinfo(&hints, hint_fields);
        hints.ai_next = NULL;
    } else phints = NULL;
    json_thing_t *response = json_make_object();
    if (reqid)
        json_add_to_object(response, "reqid", json_clone(reqid));
    const char *s;
    char *node, *service;
    if (json_object_get_string(fields, "node", &s))
        node = charstr_url_decode(s, true, NULL);
    else assert(false);
    if (json_object_get_string(fields, "service", &s))
        service = charstr_url_decode(s, true, NULL);
    else service = NULL;
    FSTRACE(FSADNS_SERVE_GETADDRINFO_START, shared->dns->uid);
    int err = getaddrinfo(node, service, phints, &res);
    if (err) {
        FSTRACE(FSADNS_SERVE_GETADDRINFO_FAIL, shared->dns->uid,
                trace_getaddrinfo_error, &err);
        construct_resolve_address_fail(response, err, errno);
    } else {
        FSTRACE(FSADNS_SERVE_GETADDRINFO, shared->dns->uid);
        construct_resolve_address_resp(response, res);
        freeaddrinfo(res);
    }
    fsfree(node);
    fsfree(service);
    fsfree(hints.ai_canonname);
    fsfree(hints.ai_addr);
    return response;
}

static void construct_resolve_name_fail(json_thing_t *resp, int err, int syserr)
{
    json_thing_t *fields = json_make_object();
    json_add_to_object(resp, "resolve_name_fail", fields);
    json_add_to_object(fields, "error", json_make_integer(err));
    if (err == EAI_SYSTEM)
        json_add_to_object(fields, "errno", json_make_integer(syserr));
}

static void construct_resolve_name_resp(json_thing_t *resp, const char *host,
                                        const char *serv)
{
    json_thing_t *fields = json_make_object();
    json_add_to_object(resp, "resolve_name_resp", fields);
    json_add_to_object(fields, "host",
                       json_adopt_string(charstr_url_encode(host)));
    json_add_to_object(fields, "serv",
                       json_adopt_string(charstr_url_encode(serv)));
}

FSTRACE_DECL(FSADNS_SERVE_GETNAMEINFO_START, "UID=%64u PID=%P TID=%T");
FSTRACE_DECL(FSADNS_SERVE_GETNAMEINFO,
             "UID=%64u PID=%P TID=%T HOST=%s SERV=%s");
FSTRACE_DECL(FSADNS_SERVE_GETNAMEINFO_FAIL,
             "UID=%64u PID=%P TID=%T ERR=%I ERRNO=%e");

static json_thing_t *resolve_name(shared_t *shared, json_thing_t *reqid,
                                  json_thing_t *fields)
{
    const char *addr_base64;
    if (!json_object_get_string(fields, "addr", &addr_base64))
        assert(false);
    size_t addrlen;
    struct sockaddr *addr = base64_decode_simple(addr_base64, &addrlen);
    long long flags;
    if (!json_object_get_integer(fields, "flags", &flags))
        assert(false);
    json_thing_t *response = json_make_object();
    if (reqid)
        json_add_to_object(response, "reqid", json_clone(reqid));
    char host[2000], serv[2000];
    FSTRACE(FSADNS_SERVE_GETNAMEINFO_START, shared->dns->uid);
    int err = getnameinfo(addr, addrlen, host, sizeof host, serv, sizeof serv,
                          (int) flags);
    fsfree(addr);
    if (err) {
        FSTRACE(FSADNS_SERVE_GETNAMEINFO_FAIL, shared->dns->uid,
                trace_getaddrinfo_error, &err);
        construct_resolve_name_fail(response, err, errno);
    } else {
        FSTRACE(FSADNS_SERVE_GETNAMEINFO, shared->dns->uid, host, serv);
        construct_resolve_name_resp(response, host, serv);
    }
    return response;
}

FSTRACE_DECL(FSADNS_SERVE_RESOLVE, "UID=%64u PID=%P TID=%T PDU=%I");
FSTRACE_DECL(FSADNS_SERVE_RESOLVE_CONFUSED, "UID=%64u PID=%P TID=%T");

static json_thing_t *resolve(shared_t *shared, json_thing_t *request)
{
    FSTRACE(FSADNS_SERVE_RESOLVE, shared->dns->uid, json_trace, request);
    assert(json_thing_type(request) == JSON_OBJECT);
    json_thing_t *reqid = json_object_get(request, "reqid");
    json_thing_t *fields;
    if (json_object_get_object(request, "resolve_address_req", &fields))
        return resolve_address(shared, reqid, fields);
    if (json_object_get_object(request, "resolve_name_req", &fields))
        return resolve_name(shared, reqid, fields);
    FSTRACE(FSADNS_SERVE_RESOLVE_CONFUSED, shared->dns->uid);
    assert(false);
}

static void lock(shared_t *shared)
{
    pthread_mutex_lock(&shared->lock);
}

static void notify(shared_t *shared)
{
    pthread_cond_signal(&shared->cond);
}

static void await_notification(shared_t *shared)
{
    pthread_cond_wait(&shared->cond, &shared->lock);
}

static void unlock(shared_t *shared)
{
    pthread_mutex_unlock(&shared->lock);
}

FSTRACE_DECL(FSADNS_SERVE_FATAL_ERROR, "UID=%64u PID=%P TID=%T");

static void fatal()
{
    _exit(1);
}

static void serve_requests(shared_t *shared);

FSTRACE_DECL(FSADNS_SERVE_NOTIFY, "UID=%64u PID=%P TID=%T");
FSTRACE_DECL(FSADNS_SERVE_BUSY, "UID=%64u PID=%P TID=%T");
FSTRACE_DECL(FSADNS_SERVE_PTHREAD_CREATE_FAIL,
             "UID=%64u PID=%P TID=%T ERRNO=%E");
FSTRACE_DECL(FSADNS_SERVE_PTHREAD_CREATE,
             "UID=%64u PID=%P TID=%T READY=%u QUOTA=%u");

static void thread_probe_locked(shared_t *shared)
{
    if (shared->ready) {
        notify(shared);
        FSTRACE(FSADNS_SERVE_NOTIFY, shared->dns->uid);
        return;
    }
    if (!shared->quota_remaining) {
        FSTRACE(FSADNS_SERVE_NOTIFY, shared->dns->uid);
        return;
    }
    pthread_t t;
    int err = pthread_create(&t, NULL, (void *) serve_requests, shared);
    if (err)
        FSTRACE(FSADNS_SERVE_PTHREAD_CREATE_FAIL, shared->dns->uid, err);
    else {
        shared->ready++;
        shared->quota_remaining--;
        FSTRACE(FSADNS_SERVE_PTHREAD_CREATE, shared->dns->uid, shared->ready,
                shared->quota_remaining);
    }
}

FSTRACE_DECL(FSADNS_SERVE_REQUESTS, "UID=%64u PID=%P TID=%T");
FSTRACE_DECL(FSADNS_SERVE_REQUESTS_LOCKED, "UID=%64u PID=%P TID=%T");
FSTRACE_DECL(FSADNS_SERVE_REQUESTS_AWAIT, "UID=%64u PID=%P TID=%T");
FSTRACE_DECL(FSADNS_SERVE_REQUESTS_NOTIFIED, "UID=%64u PID=%P TID=%T");
FSTRACE_DECL(FSADNS_SERVE_REQUESTS_FAIL, "UID=%64u PID=%P TID=%T ERRNO=%e");

static void serve_requests(shared_t *shared)
{
    fsadns_t *dns = shared->dns;
    FSTRACE(FSADNS_SERVE_REQUESTS, dns->uid);
    lock(shared);
    FSTRACE(FSADNS_SERVE_REQUESTS_LOCKED, dns->uid);
    for (;;) {
        json_thing_t *request = json_conn_receive(shared->conn);
        if (request) {
            shared->ready--;
            thread_probe_locked(shared);
            unlock(shared);
            json_thing_t *response = resolve(shared, request);
            lock(shared);
            json_conn_send(shared->conn, response);
            json_destroy_thing(request);
            json_destroy_thing(response);
            shared->ready++;
        } else if (errno == EAGAIN) {
            FSTRACE(FSADNS_SERVE_REQUESTS_AWAIT, dns->uid);
            await_notification(shared);
            FSTRACE(FSADNS_SERVE_REQUESTS_NOTIFIED, dns->uid);
        }
        else {
            FSTRACE(FSADNS_SERVE_REQUESTS_FAIL, dns->uid);
            fatal();
        }
    }
}

FSTRACE_DECL(FSADNS_SERVE_PROBE, "UID=%64u PID=%P TID=%T");

static void probe_server(shared_t *shared)
{
    FSTRACE(FSADNS_SERVE_PROBE, shared->dns->uid);
    lock(shared);
    thread_probe_locked(shared);
    unlock(shared);
}

FSTRACE_DECL(FSADNS_SERVING, "UID=%64u PID=%P");
FSTRACE_DECL(FSADNS_SERVING_ASYNC_FAIL, "UID=%64u PID=%P ERRNO=%e");

static void serve(int fd, fsadns_t *dns)
{
    FSTRACE(FSADNS_SERVING, dns->uid);
    async_t *async = make_async();
    shared_t shared = {
        .dns = dns,
        .async = async,
        .conn = open_json_conn(async, tcp_adopt_connection(async, fd),
                               100000),
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .ready = 0,
        .quota_remaining = dns->max_parallel,
    };
    action_1 probe_cb = { &shared, (act_1) probe_server };
    json_conn_register_callback(shared.conn, probe_cb);
    lock(&shared);
    thread_probe_locked(&shared);
    for (;;) {
        int status =
            async_loop_protected(async, (void *) lock, (void *) unlock,
                                 &shared);
        if (status >= 0 || errno != EINTR) {
            FSTRACE(FSADNS_SERVING_ASYNC_FAIL, dns->uid);
            fatal();
        }
    }
}

static void dns_probe(fsadns_t *dns)
{
    if (!dns->async)
        return;
    list_elem_t *e;
    for (e = list_get_first(dns->queries); e; e = list_next(e)) {
        fsadns_query_t *q = (fsadns_query_t *) list_elem_get_value(e);
        switch (q->state) {
            case QUERY_REQUESTED_ADDRESS:
            case QUERY_REQUESTED_NAME:
            case QUERY_CANCELED:
                action_1_perf(q->probe);
                return;
            default:
                ;
        }
    }
}

FSTRACE_DECL(FSADNS_CREATE, "UID=%64u PTR=%p ASYNC=%p MAX-PAR=%u");
FSTRACE_DECL(FSADNS_CREATE_SOCKETPAIR_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(FSADNS_CREATE_FORK_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(FSADNS_CREATE_STARTED, "UID=%64u CHILD-PID=%d JSON-CONN=%p");
FSTRACE_DECL(FSADNS_CHILD_EXIT, "UID=%64u PID=%P");

fsadns_t *fsadns_make_resolver(async_t *async, unsigned max_parallel,
                               action_1 post_fork_cb)
{
    fsadns_t *dns = fsalloc(sizeof *dns);
    dns->uid = fstrace_get_unique_id();
    FSTRACE(FSADNS_CREATE, dns->uid, dns, async, max_parallel);
    dns->error = 0;
    dns->async = async;
    assert(max_parallel >= 1);
    dns->max_parallel = max_parallel;
    int fd[2];
    int status = socketpair(AF_UNIX, SOCK_STREAM, 0, fd);
    if (status < 0) {
        FSTRACE(FSADNS_CREATE_SOCKETPAIR_FAIL, dns->uid);
        fsfree(dns);
        return NULL;
    }
    list_t *fds_to_keep = make_list();
    list_append(fds_to_keep, as_integer(0));
    list_append(fds_to_keep, as_integer(1));
    list_append(fds_to_keep, as_integer(2));
    list_append(fds_to_keep, as_integer(fd[1]));
    dns->child = unixkit_fork(fds_to_keep);
    if (dns->child < 0) {
        FSTRACE(FSADNS_CREATE_FORK_FAIL, dns->uid);
        close(fd[0]);
        close(fd[1]);
        fsfree(dns);
        return NULL;
    }
    if (dns->child > 0) {
        /* parent */
        close(fd[1]);
        dns->conn = open_json_conn(dns->async,
                                   tcp_adopt_connection(dns->async, fd[0]),
                                                        100000);
        FSTRACE(FSADNS_CREATE_STARTED, dns->uid, (int) dns->child, dns->conn);
        json_conn_register_callback(dns->conn,
                                    (action_1) { dns, (act_1) dns_probe });
        dns->query_map = make_hash_table(10000, query_hash, query_cmp);
        dns->queries = make_list();
        return dns;
    }
    /* child */
    action_1_perf(post_fork_cb);
    serve(fd[1], dns);
    FSTRACE(FSADNS_CHILD_EXIT, dns->uid);
    _exit(0);
}

static const char *trace_state(void *pstate)
{
    switch (*(query_state_t *) pstate) {
        case QUERY_REQUESTED_ADDRESS:
            return "QUERY_REQUESTED_ADDRESS";
        case QUERY_REQUESTED_NAME:
            return "QUERY_REQUESTED_NAME";
        case QUERY_CANCELED:
            return "QUERY_CANCELED";
        case QUERY_REPLIED_ADDRESS:
            return "QUERY_REPLIED_ADDRESS";
        case QUERY_REPLIED_NAME:
            return "QUERY_REPLIED_NAME";
        case QUERY_CONSUMED:
            return "QUERY_CONSUMED";
        case QUERY_ERRORED:
            return "QUERY_ERRORED";
        case QUERY_ZOMBIE:
            return "QUERY_ZOMBIE";
        default:
            return "?";
    }
}

FSTRACE_DECL(FSADNS_QUERY_SET_STATE, "UID=%64u OLD=%I NEW=%I");

static void set_query_state(fsadns_query_t *query, query_state_t state)
{
    FSTRACE(FSADNS_QUERY_SET_STATE, query->uid,
            trace_state, &query->state, trace_state, &state);
    query->state = state;
}

FSTRACE_DECL(FSADNS_QUERY_DESTROY, "UID=%64u");

static void destroy_query(fsadns_query_t *query)
{
    FSTRACE(FSADNS_QUERY_DESTROY, query->uid);
    fsadns_t *dns = query->dns;
    switch (query->state) {
        case QUERY_REPLIED_ADDRESS:
            fsadns_freeaddrinfo(query->address.info);
            break;
        case QUERY_REPLIED_NAME:
            fsfree(query->name.host);
            fsfree(query->name.serv);
            break;
        default:
            ;
    }
    list_remove(dns->queries, query->loc);
    destroy_hash_element(hash_table_pop(dns->query_map, &query->uid));
    set_query_state(query, QUERY_ZOMBIE);
    async_wound(dns->async, query);
}

FSTRACE_DECL(FSADNS_DESTROY, "UID=%64u");
FSTRACE_DECL(FSADNS_DESTROY_WAIT_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(FSADNS_DESTROY_CHILD_DEAD, "UID=%64u");

void fsadns_destroy_resolver(fsadns_t *dns)
{
    FSTRACE(FSADNS_DESTROY, dns->uid);
    assert(dns->async != NULL);
    while (!list_empty(dns->queries))
        destroy_query((fsadns_query_t *) list_get_first(dns->queries));
    destroy_list(dns->queries);
    destroy_hash_table(dns->query_map);
    kill(dns->child, SIGTERM);
    if (waitpid(dns->child, NULL, 0) < 0)
        FSTRACE(FSADNS_DESTROY_WAIT_FAIL, dns->uid);
    else FSTRACE(FSADNS_DESTROY_CHILD_DEAD, dns->uid);
    json_conn_close(dns->conn);
    async_wound(dns->async, dns);
    dns->async = NULL;
}

static fsadns_query_t *make_address_query(fsadns_t *dns, action_1 probe)
{
    fsadns_query_t *query = fsalloc(sizeof *query);
    query->dns = dns;
    query->probe = probe;
    query->uid = fstrace_get_unique_id();
    query->state = QUERY_REQUESTED_ADDRESS;
    (void) hash_table_put(dns->query_map, &query->uid, query);
    query->loc = list_append(dns->queries, query);
    return query;
}

FSTRACE_DECL(FSADNS_ADDRESS_QUERY_CREATE,
             "UID=%64u PTR=%p DNS=%64u NODE=%s SERVICE=%s");
FSTRACE_DECL(FSADNS_ADDRESS_QUERY_NO_HINTS, "UID=%64u");
FSTRACE_DECL(FSADNS_ADDRESS_QUERY_HINTS,
             "UID=%64u FLG=0x%x FAM=%d SKTYP=%d PROT=%d ADDR=%a CANON=%s");

fsadns_query_t *fsadns_resolve(fsadns_t *dns,
                               const char *node, const char *service,
                               const struct addrinfo *hints,
                               action_1 probe)
{
    if (dns->error) {
        errno = dns->error;
        return NULL;
    }
    fsadns_query_t *query = make_address_query(dns, probe);
    FSTRACE(FSADNS_ADDRESS_QUERY_CREATE,
            query->uid, query, dns->uid, node, service);
    json_thing_t *request = json_make_object();
    json_add_to_object(request, "reqid", json_make_unsigned(query->uid));
    json_thing_t *fields = json_make_object();
    json_add_to_object(request, "resolve_address_req", fields);
    assert(node);
    json_add_to_object(fields, "node",
                       json_adopt_string(charstr_url_encode(node)));
    if (service)
        json_add_to_object(fields, "service",
                           json_adopt_string(charstr_url_encode(service)));
    if (hints) {
        FSTRACE(FSADNS_ADDRESS_QUERY_HINTS, query->uid,
                hints->ai_flags, hints->ai_family, hints->ai_socktype,
                hints->ai_protocol, hints->ai_addr, hints->ai_addrlen,
                hints->ai_flags & AI_CANONNAME ? hints->ai_canonname : NULL);
        json_add_to_object(fields, "hints", construct_addrinfo(hints));
    } else FSTRACE(FSADNS_ADDRESS_QUERY_NO_HINTS, query->uid);
    json_conn_send(dns->conn, request);
    json_destroy_thing(request);
    return query;
}

FSTRACE_DECL(FSADNS_PROPAGATE_ERROR, "UID=%64u ERRNO=%E");

static int propagate_error(fsadns_query_t *query, int error)
{
    FSTRACE(FSADNS_PROPAGATE_ERROR, query->uid, error);
    fsadns_t *dns = query->dns;
    list_elem_t *e;
    for (e = list_get_first(dns->queries); e; e = list_next(e)) {
        fsadns_query_t *q = (fsadns_query_t *) list_elem_get_value(e);
        switch (q->state) {
            case QUERY_REQUESTED_ADDRESS:
            case QUERY_REQUESTED_NAME:
                if (q != query) {
                    set_query_state(q, QUERY_ERRORED);
                    q->error.err = EAI_SYSTEM;
                    q->error.err_no = error;
                    async_execute(dns->async, query->probe);
                }
                break;
            default:
                ;
        }
    }
    destroy_query(query);
    errno = dns->error = error;
    return EAI_SYSTEM;
}

FSTRACE_DECL(FSADNS_DECODE_ERROR, "UID=%64u LINE=%L");

static bool get_response_reqid(fsadns_t *dns, json_thing_t *response,
                               uint64_t *reqid)
{
    unsigned long long n;
    if (json_thing_type(response) != JSON_OBJECT ||
        !json_object_get_unsigned(response, "reqid", &n)) {
        FSTRACE(FSADNS_DECODE_ERROR, dns->uid);
        return false;
    }
    *reqid = n;
    return true;
}

FSTRACE_DECL(FSADNS_EMPTY_RESPONSE, "UID=%64u");
FSTRACE_DECL(FSADNS_ALLOC_ADDR_INFO, "UID=%64u INFO=%p");
FSTRACE_DECL(FSADNS_GOT_INFO,
             "UID=%64u FLG=0x%x FAM=%d SKTYP=%d PROT=%d ADDR=%a CANON=%s")
FSTRACE_DECL(FSADNS_GOOD_ADDRESS_RESPONSE, "UID=%64u");

static bool parse_address_response(fsadns_t *dns, json_thing_t *response,
                                   struct addrinfo **res)
{
    json_thing_t *fields;
    if (!json_object_get_object(response, "resolve_address_resp", &fields))
        return false;
    json_thing_t *responses;
    if (!json_object_get_array(fields, "responses", &responses)) {
        FSTRACE(FSADNS_EMPTY_RESPONSE, dns->uid);
        *res = NULL;
        return true;
    }
    json_element_t *e;
    for (e = json_array_first(responses); e; e = json_element_next(e)) {
        struct addrinfo *ip = *res = fscalloc(1, sizeof *ip);
        FSTRACE(FSADNS_ALLOC_ADDR_INFO, dns->uid, ip);
        deconstruct_addrinfo(ip, json_element_value(e));
        FSTRACE(FSADNS_GOT_INFO, dns->uid, ip->ai_flags, ip->ai_family,
                ip->ai_socktype, ip->ai_protocol, ip->ai_addr, ip->ai_addrlen,
                ip->ai_canonname);
        res = &ip->ai_next;
    }
    FSTRACE(FSADNS_GOOD_ADDRESS_RESPONSE, dns->uid);
    *res = NULL;
    return true;
}

FSTRACE_DECL(FSADNS_GOOD_ADDRESS_FAILURE, "UID=%64u ERR=%d ERRNO=%d");

static bool parse_address_failure(fsadns_t *dns, json_thing_t *response,
                                  int *error, int *err_no)
{
    json_thing_t *fields;
    if (!json_object_get_object(response, "resolve_address_fail", &fields))
        return false;
    long long err;
    if (!json_object_get_integer(fields, "error", &err))
        assert(false);
    *error = err;
    if (json_object_get_integer(fields, "errno", &err))
        *err_no = err;
    else *err_no = -1;
    FSTRACE(FSADNS_GOOD_ADDRESS_FAILURE, dns->uid, *error, *err_no);
    return true;
}

FSTRACE_DECL(FSADNS_GOOD_NAME_RESPONSE, "UID=%64u");

static bool parse_name_response(fsadns_t *dns, json_thing_t *response,
                                char **host, char **serv)
{
    json_thing_t *fields;
    const char *host_field, *serv_field;
    if (!json_object_get_object(response, "resolve_name_resp", &fields) ||
        !json_object_get_string(fields, "host", &host_field) ||
        !json_object_get_string(fields, "serv", &serv_field)) {
        FSTRACE(FSADNS_DECODE_ERROR, dns->uid);
        return false;
    }
    *host = charstr_url_decode(host_field, true, NULL);
    *serv = charstr_url_decode(serv_field, true, NULL);
    FSTRACE(FSADNS_GOOD_NAME_RESPONSE, dns->uid);
    return true;
}

FSTRACE_DECL(FSADNS_GOOD_NAME_FAILURE, "UID=%64u ERR=%d ERRNO=%d");

static bool parse_name_failure(fsadns_t *dns, json_thing_t *response,
                               int *error, int *err_no)
{
    json_thing_t *fields;
    if (!json_object_get_object(response, "resolve_name_fail", &fields))
        return false;
    long long err;
    if (!json_object_get_integer(fields, "error", &err))
        assert(false);
    *error = err;
    if (json_object_get_integer(fields, "errno", &err))
        *err_no = err;
    else *err_no = -1;
    FSTRACE(FSADNS_GOOD_NAME_FAILURE, dns->uid, *error, *err_no);
    return true;
}

FSTRACE_DECL(FSADNS_QUERY_CONFUSED, "UID=%64u OTHER=%64u");
FSTRACE_DECL(FSADNS_RELAY_ADDRESS_RESPONSE, "UID=%64u OTHER=%64u");
FSTRACE_DECL(FSADNS_RELAY_NAME_RESPONSE, "UID=%64u OTHER=%64u");
FSTRACE_DECL(FSADNS_CLEAR_CANCELED, "UID=%64u OTHER=%64u");

static bool relay_response(fsadns_query_t *query, json_thing_t *response,
                           uint64_t reqid)
{
    fsadns_t *dns = query->dns;
    hash_elem_t *e = hash_table_get(dns->query_map, &reqid);
    if (!e) {
        FSTRACE(FSADNS_QUERY_CONFUSED, query->uid, query->uid);
        return false;
    }
    fsadns_query_t *other = (fsadns_query_t *) hash_elem_get_value(e);
    switch (other->state) {
        case QUERY_REQUESTED_ADDRESS:
            FSTRACE(FSADNS_RELAY_ADDRESS_RESPONSE, query->uid, other->uid);
            {
                struct addrinfo *info;
                if (parse_address_response(dns, response, &info)) {
                    set_query_state(other, QUERY_REPLIED_ADDRESS);
                    other->address.info = info;
                    async_execute(dns->async, other->probe);
                    return true;
                }
                int err, err_no;
                if (parse_address_failure(dns, response, &err, &err_no)) {
                    set_query_state(other, QUERY_ERRORED);
                    other->error.err = err;
                    other->error.err_no = err_no;
                    async_execute(dns->async, other->probe);
                    return true;
                }
            }
            FSTRACE(FSADNS_DECODE_ERROR, dns->uid);
            return false;
        case QUERY_REQUESTED_NAME:
            FSTRACE(FSADNS_RELAY_NAME_RESPONSE, query->uid, other->uid);
            {
                char *host, *serv;
                if (parse_name_response(dns, response, &host, &serv)) {
                    set_query_state(other, QUERY_REPLIED_NAME);
                    other->name.host = host;
                    other->name.serv = serv;
                    async_execute(dns->async, other->probe);
                    return true;
                }
                int err, err_no;
                if (parse_name_failure(dns, response, &err, &err_no)) {
                    set_query_state(other, QUERY_ERRORED);
                    other->error.err = err;
                    other->error.err_no = err_no;
                    async_execute(dns->async, other->probe);
                    return true;
                }
            }
            FSTRACE(FSADNS_DECODE_ERROR, dns->uid);
            return false;
        case QUERY_CANCELED:
            FSTRACE(FSADNS_CLEAR_CANCELED, query->uid, other->uid);
            destroy_query(query);
            return true;
        default:
            FSTRACE(FSADNS_QUERY_CONFUSED, query->uid, other->uid);
            return false;
    }
}

FSTRACE_DECL(FSADNS_CHECK_ADDRESS_QUERY_RESPONSE, "UID=%64u");
FSTRACE_DECL(FSADNS_CHECK_ADDRESS_QUERY_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(FSADNS_CHECK_ADDRESS_QUERY_PDU, "UID=%64u PDU=%I");
FSTRACE_DECL(FSADNS_ADDRESS_QUERY_MATCH, "UID=%64u");

static int check_address_query_response(fsadns_query_t *query,
                                        struct addrinfo **res)
{
    FSTRACE(FSADNS_CHECK_ADDRESS_QUERY_RESPONSE, query->uid);
    fsadns_t *dns = query->dns;
    for (;;) {
        json_thing_t *response = json_conn_receive(dns->conn);
        if (!response) {
            FSTRACE(FSADNS_CHECK_ADDRESS_QUERY_FAIL, query->uid);
            switch (errno) {
                case 0:
                    return propagate_error(query, EPROTO);
                case EAGAIN:
                    return EAI_SYSTEM;
                default:
                    return propagate_error(query, errno);
            }
        }
        FSTRACE(FSADNS_CHECK_ADDRESS_QUERY_PDU,
                query->uid, json_trace, response);
        uint64_t reqid;
        if (!get_response_reqid(dns, response, &reqid)) {
            json_destroy_thing(response);
            return propagate_error(query, EPROTO);
        }
        if (reqid == query->uid) {
            FSTRACE(FSADNS_ADDRESS_QUERY_MATCH, query->uid);
            if (parse_address_response(dns, response, res)) {
                json_destroy_thing(response);
                destroy_query(query);
                async_execute(dns->async,
                              (action_1) { dns, (act_1) dns_probe });
                return 0;
            }
            int err, err_no;
            if (parse_address_failure(dns, response, &err, &err_no)) {
                json_destroy_thing(response);
                destroy_query(query);
                async_execute(dns->async,
                              (action_1) { dns, (act_1) dns_probe });
                if (err_no >= 0)
                    errno = err_no;
                return err;
            }
            FSTRACE(FSADNS_DECODE_ERROR, dns->uid);
            json_destroy_thing(response);
            return propagate_error(query, EPROTO);
        }
        if (!relay_response(query, response, reqid)) {
            json_destroy_thing(response);
            return propagate_error(query, EPROTO);
        }
        json_destroy_thing(response);
    }
}

FSTRACE_DECL(FSADNS_ADDRESS_QUERY_REPLIED, "UID=%64u INFO=%p");
FSTRACE_DECL(FSADNS_ADDRESS_QUERY_ERRORED, "UID=%64u ERRNO=%E");
FSTRACE_DECL(FSADNS_POSTHUMOUS_ADDRESS_CHECK, "UID=%64u");

int fsadns_check(fsadns_query_t *query, struct addrinfo **res)
{
    fsadns_t *dns = query->dns;
    switch (query->state) {
        case QUERY_REQUESTED_ADDRESS:
            return check_address_query_response(query, res);
        case QUERY_REPLIED_ADDRESS:
            FSTRACE(FSADNS_ADDRESS_QUERY_REPLIED,
                    query->uid, query->address.info);
            *res = query->address.info;
            set_query_state(query, QUERY_CONSUMED);
            destroy_query(query);
            async_execute(dns->async, (action_1) { dns, (act_1) dns_probe });
            return 0;
        case QUERY_ERRORED:
            FSTRACE(FSADNS_ADDRESS_QUERY_ERRORED, query->uid, dns->error);
            {
                int err = query->error.err;
                if (query->error.err_no >= 0)
                    errno = query->error.err_no;
                destroy_query(query);
                return err;
            }
        case QUERY_ZOMBIE:
            FSTRACE(FSADNS_POSTHUMOUS_ADDRESS_CHECK, query->uid);
            errno = EINVAL;
            return EAI_SYSTEM;
        default:
            assert(false);
    }
}

FSTRACE_DECL(FSADNS_FREE_ADDR_INFO, "INFO=%p");

void fsadns_freeaddrinfo(struct addrinfo *res)
{
    FSTRACE(FSADNS_FREE_ADDR_INFO, res);
    while (res) {
        fsfree(res->ai_addr);
        fsfree(res->ai_canonname);
        struct addrinfo *next = res->ai_next;
        fsfree(res);
        res = next;
    }
}

FSTRACE_DECL(FSADNS_MARK_CANCEL, "UID=%64u");
FSTRACE_DECL(FSADNS_CANCEL, "UID=%64u");
FSTRACE_DECL(FSADNS_POSTHUMOUS_CANCEL, "UID=%64u");

void fsadns_cancel(fsadns_query_t *query)
{
    switch (query->state) {
        case QUERY_REQUESTED_ADDRESS:
        case QUERY_REQUESTED_NAME:
            FSTRACE(FSADNS_MARK_CANCEL, query->uid);
            set_query_state(query, QUERY_CANCELED);
            break;
        case QUERY_REPLIED_ADDRESS:
        case QUERY_REPLIED_NAME:
        case QUERY_ERRORED:
            FSTRACE(FSADNS_CANCEL, query->uid);
            destroy_query(query);
            break;
        case QUERY_ZOMBIE:
            FSTRACE(FSADNS_POSTHUMOUS_CANCEL, query->uid);
            break;
        default:
            assert(false);
    }
}

static fsadns_query_t *make_name_query(fsadns_t *dns, action_1 probe)
{
    fsadns_query_t *query = fsalloc(sizeof *query);
    query->dns = dns;
    query->probe = probe;
    query->uid = fstrace_get_unique_id();
    query->state = QUERY_REQUESTED_NAME;
    (void) hash_table_put(dns->query_map, &query->uid, query);
    query->loc = list_append(dns->queries, query);
    return query;
}

FSTRACE_DECL(FSADNS_NAME_QUERY_CREATE, "UID=%64u PTR=%p DNS=%64u ADDRESS=%a");

fsadns_query_t *fsadns_resolve_name(fsadns_t *dns,
                                    const struct sockaddr *addr,
                                    socklen_t addrlen,
                                    int flags,
                                    action_1 probe)
{
    if (dns->error) {
        errno = dns->error;
        return NULL;
    }
    fsadns_query_t *query = make_name_query(dns, probe);
    FSTRACE(FSADNS_NAME_QUERY_CREATE, query->uid, query, dns->uid,
            addr, addrlen);
    json_thing_t *request = json_make_object();
    json_add_to_object(request, "reqid", json_make_unsigned(query->uid));
    json_thing_t *fields = json_make_object();
    json_add_to_object(request, "resolve_name_req", fields);
    char *base64_encoded = base64_encode_simple(addr, addrlen);
    json_add_to_object(fields, "addr", json_adopt_string(base64_encoded));
    json_add_to_object(fields, "flags", json_make_integer(flags));
    json_conn_send(dns->conn, request);
    json_destroy_thing(request);
    return query;
}

static void move_name(char *name, socklen_t len, char *field)
{
    if (name && len > 0) {
        strncpy(name, field, len);
        name[len - 1] = '\0';
    }
    fsfree(field);
}

FSTRACE_DECL(FSADNS_CHECK_NAME_QUERY_RESPONSE, "UID=%64u");
FSTRACE_DECL(FSADNS_CHECK_NAME_QUERY_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(FSADNS_CHECK_NAME_QUERY_PDU, "UID=%64u PDU=%I");
FSTRACE_DECL(FSADNS_NAME_QUERY_MATCH, "UID=%64u");

static int check_name_query_response(fsadns_query_t *query,
                                     char *host, socklen_t hostlen,
                                     char *serv, socklen_t servlen)
{
    FSTRACE(FSADNS_CHECK_NAME_QUERY_RESPONSE, query->uid);
    fsadns_t *dns = query->dns;
    for (;;) {
        json_thing_t *response = json_conn_receive(dns->conn);
        if (!response) {
            FSTRACE(FSADNS_CHECK_NAME_QUERY_FAIL, query->uid);
            switch (errno) {
                case 0:
                    return propagate_error(query, EPROTO);
                case EAGAIN:
                    return EAI_SYSTEM;
                default:
                    return propagate_error(query, errno);
            }
        }
        FSTRACE(FSADNS_CHECK_NAME_QUERY_PDU, query->uid, json_trace, response);
        uint64_t reqid;
        if (!get_response_reqid(dns, response, &reqid)) {
            json_destroy_thing(response);
            return propagate_error(query, EPROTO);
        }
        if (reqid == query->uid) {
            FSTRACE(FSADNS_NAME_QUERY_MATCH, query->uid);
            char *host_field, *serv_field;
            if (parse_name_response(dns, response, &host_field, &serv_field)) {
                json_destroy_thing(response);
                destroy_query(query);
                move_name(host, hostlen, host_field);
                move_name(serv, servlen, serv_field);
                async_execute(dns->async, (action_1) { dns, (act_1) dns_probe });
                return 0;
            }
            int err, err_no;
            if (parse_name_failure(dns, response, &err, &err_no)) {
                json_destroy_thing(response);
                destroy_query(query);
                async_execute(dns->async,
                              (action_1) { dns, (act_1) dns_probe });
                if (err_no >= 0)
                    errno = err_no;
                return err;
            }
            FSTRACE(FSADNS_DECODE_ERROR, dns->uid);
            json_destroy_thing(response);
            return propagate_error(query, EPROTO);
        }
        if (!relay_response(query, response, reqid)) {
            json_destroy_thing(response);
            return propagate_error(query, EPROTO);
        }
        json_destroy_thing(response);
    }
}

FSTRACE_DECL(FSADNS_NAME_QUERY_REPLIED, "UID=%64u HOST=%s SERV=%s");
FSTRACE_DECL(FSADNS_NAME_QUERY_ERRORED, "UID=%64u ERRNO=%E");
FSTRACE_DECL(FSADNS_POSTHUMOUS_NAME_CHECK, "UID=%64u");

int fsadns_check_name(fsadns_query_t *query,
                      char *host, socklen_t hostlen,
                      char *serv, socklen_t servlen)
{
    fsadns_t *dns = query->dns;
    switch (query->state) {
        case QUERY_REQUESTED_NAME:
            return check_name_query_response(query,
                                             host, hostlen, serv, servlen);
        case QUERY_REPLIED_NAME:
            FSTRACE(FSADNS_NAME_QUERY_REPLIED, query->uid, query->name.host,
                    query->name.serv);
            move_name(host, hostlen, query->name.host);
            move_name(serv, servlen, query->name.serv);
            set_query_state(query, QUERY_CONSUMED);
            destroy_query(query);
            async_execute(dns->async, (action_1) { dns, (act_1) dns_probe });
            return 0;
        case QUERY_ERRORED:
            FSTRACE(FSADNS_NAME_QUERY_ERRORED, query->uid, dns->error);
            {
                int err = query->error.err;
                if (query->error.err_no >= 0)
                    errno = query->error.err_no;
                destroy_query(query);
                return err;
            }
        case QUERY_ZOMBIE:
            FSTRACE(FSADNS_POSTHUMOUS_NAME_CHECK, query->uid);
            errno = EINVAL;
            return EAI_SYSTEM;
        default:
            assert(false);
    }
}
