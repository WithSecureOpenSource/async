#include "tcp_client.h"

#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <fsdyn/fsalloc.h>
#include <fsdyn/list.h>
#include <fstrace.h>

#include "async_version.h"
#include "drystream.h"
#include "fsadns.h"

typedef enum {
    TCP_CLIENT_RESOLVING,  /* performing address resolution */
    TCP_CLIENT_CONNECTING, /* establishing a connection */
    TCP_CLIENT_CONNECTED,  /* connection established but not relayed to user */
    TCP_CLIENT_NOTIFIED,   /* connection established and user notified */
    TCP_CLIENT_RELAYED,    /* connection established and relayed to user */
    TCP_CLIENT_ZOMBIE
} tcp_client_state_t;

struct tcp_client {
    async_t *async;
    uint64_t uid;
    tcp_client_state_t state;
    unsigned port;            /* TCP_CLIENT_RESOLVING */
    fsadns_query_t *query;    /* TCP_CLIENT_RESOLVING */
    list_t *candidates;       /* of conn_candidate_t; TCP_CLIENT_CONNECTING */
    tcp_conn_t *chosen;       /* TCP_CLIENT_CONNECTED */
    action_1 choice_callback; /* to alert the user */
};

typedef struct {
    uint64_t uid;
    tcp_client_t *client;
    tcp_conn_t *conn;
} conn_candidate_t;

typedef struct {
    struct sockaddr *address;
    socklen_t addrlen;
} socket_address_t;

FSTRACE_DECL(ASYNC_TCP_CLIENT_RESOLUTION, "UID=%64u ADDR=%a");

static socket_address_t *resolve_ipv4(tcp_client_t *client,
                                      const struct addrinfo *res, unsigned port)
{
    assert(res->ai_addrlen == sizeof(struct sockaddr_in));
    socket_address_t *sa = fsalloc(sizeof *sa);
    sa->address = fsalloc(res->ai_addrlen);
    sa->addrlen = res->ai_addrlen;
    memcpy(sa->address, res->ai_addr, res->ai_addrlen);
    ((struct sockaddr_in *) sa->address)->sin_port = htons(port);
    FSTRACE(ASYNC_TCP_CLIENT_RESOLUTION, client->uid, sa->address,
            (socklen_t) sizeof(struct sockaddr_in));
    return sa;
}

static socket_address_t *resolve_ipv6(tcp_client_t *client,
                                      const struct addrinfo *res, unsigned port)
{
    assert(res->ai_addrlen == sizeof(struct sockaddr_in6));
    socket_address_t *sa = fsalloc(sizeof *sa);
    sa->address = fsalloc(res->ai_addrlen);
    sa->addrlen = res->ai_addrlen;
    memcpy(sa->address, res->ai_addr, res->ai_addrlen);
    ((struct sockaddr_in6 *) sa->address)->sin6_port = htons(port);
    FSTRACE(ASYNC_TCP_CLIENT_RESOLUTION, client->uid, sa->address,
            (socklen_t) sizeof(struct sockaddr_in6));
    return sa;
}

static list_t *parse_addrinfo(tcp_client_t *client, const struct addrinfo *res,
                              unsigned port)
{
    list_t *addresses = make_list();
    const struct addrinfo *r;
    for (r = res; r; r = r->ai_next) {
        switch (r->ai_family) {
            case AF_INET:
                list_append(addresses, resolve_ipv4(client, r, port));
                break;
            case AF_INET6:
                list_append(addresses, resolve_ipv6(client, r, port));
                break;
            default:;
        }
    }
    return addresses;
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
        case EAI_NONAME:
            return "EAI_NONAME";
        case EAI_SERVICE:
            return "EAI_SERVICE";
        case EAI_SOCKTYPE:
            return "EAI_SOCKTYPE";
        case EAI_SYSTEM:
            return "EAI_SYSTEM";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNC_TCP_CLIENT_GETADDRINFO, "UID=%64u HOST=%s PORT=%u");
FSTRACE_DECL(ASYNC_TCP_CLIENT_GETADDRINFO_FAIL, "UID=%64u ERR=%I ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_CLIENT_GETADDRINFO_PARSED, "UID=%64u");

static list_t *resolve_hostname(tcp_client_t *client, const char *host,
                                unsigned port)
{
    struct addrinfo *res;
    const struct addrinfo hint = {
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
    };
    FSTRACE(ASYNC_TCP_CLIENT_GETADDRINFO, client->uid, host, port);
    int status = getaddrinfo(host, NULL, &hint, &res);
    if (status) {
        FSTRACE(ASYNC_TCP_CLIENT_GETADDRINFO_FAIL, client->uid,
                trace_getaddrinfo_error, &status);
        return make_list();
    }
    list_t *addresses = parse_addrinfo(client, res, port);
    freeaddrinfo(res);
    FSTRACE(ASYNC_TCP_CLIENT_GETADDRINFO_PARSED, client->uid);
    return addresses;
}

static void free_addresses(list_t *addresses)
{
    while (!list_empty(addresses)) {
        socket_address_t *sa = (socket_address_t *) list_pop_first(addresses);
        fsfree(sa->address);
        fsfree(sa);
    }
    destroy_list(addresses);
}

static const char *trace_client_state(void *pstate)
{
    switch (*(tcp_client_state_t *) pstate) {
        case TCP_CLIENT_RESOLVING:
            return "TCP_CLIENT_RESOLVING";
        case TCP_CLIENT_CONNECTING:
            return "TCP_CLIENT_CONNECTING";
        case TCP_CLIENT_CONNECTED:
            return "TCP_CLIENT_CONNECTED";
        case TCP_CLIENT_NOTIFIED:
            return "TCP_CLIENT_NOTIFIED";
        case TCP_CLIENT_RELAYED:
            return "TCP_CLIENT_RELAYED";
        case TCP_CLIENT_ZOMBIE:
            return "TCP_CLIENT_ZOMBIE";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNC_TCP_CLIENT_SET_STATE, "UID=%64u OLD=%I NEW=%I");

static void set_client_state(tcp_client_t *client, tcp_client_state_t state)
{
    FSTRACE(ASYNC_TCP_CLIENT_SET_STATE, client->uid, trace_client_state,
            &client->state, trace_client_state, &state);
    client->state = state;
}

FSTRACE_DECL(ASYNC_TCP_CLIENT_NOTIFY, "UID=%64u");

static void notify_choice(tcp_client_t *client)
{
    if (client->state == TCP_CLIENT_ZOMBIE)
        return;

    FSTRACE(ASYNC_TCP_CLIENT_NOTIFY, client->uid);
    while (!list_empty(client->candidates)) {
        conn_candidate_t *other =
            (conn_candidate_t *) list_pop_first(client->candidates);
        /* Note: both tcp_set_output_stream() and tcp_close() will cause
         * candidate_close() to be called back. */
        if (other->conn == client->chosen)
            tcp_set_output_stream(other->conn, drystream);
        else {
            tcp_close_input_stream(other->conn);
            tcp_close(other->conn);
        }
        async_wound(client->async, other);
    }
    destroy_list(client->candidates);
    set_client_state(client, TCP_CLIENT_NOTIFIED);
    async_execute(client->async, client->choice_callback);
}

FSTRACE_DECL(ASYNC_TCP_CLIENT_SPURIOUS_CHOICE, "UID=%64u CANDIDATE=%64u");
FSTRACE_DECL(ASYNC_TCP_CLIENT_CHOOSE, "UID=%64u CANDIDATE=%64u");

static void make_choice(conn_candidate_t *candidate)
{
    tcp_client_t *client = candidate->client;
    if (client->state != TCP_CLIENT_CONNECTING) {
        FSTRACE(ASYNC_TCP_CLIENT_SPURIOUS_CHOICE, client->uid, candidate->uid);
        return;
    }
    FSTRACE(ASYNC_TCP_CLIENT_CHOOSE, client->uid, candidate->uid);
    client->chosen = candidate->conn;
    set_client_state(client, TCP_CLIENT_CONNECTED);
    action_1 notify_cb = { client, (act_1) notify_choice };
    async_execute(client->async, notify_cb);
}

static ssize_t candidate_read(void *obj, void *buf, size_t count)
{
    conn_candidate_t *candidate = obj;
    make_choice(candidate);
    errno = EAGAIN;
    return -1;
}

FSTRACE_DECL(ASYNC_TCP_CLIENT_CLOSE_CANDIDATE, "UID=%64u CANDIDATE=%64u");

static void candidate_close(void *obj)
{
    conn_candidate_t *candidate = obj;
    tcp_client_t *client = candidate->client;
    FSTRACE(ASYNC_TCP_CLIENT_CLOSE_CANDIDATE, client->uid, candidate->uid);
    make_choice(candidate);
}

static void candidate_register_callback(void *obj, action_1 action) {}

static void candidate_unregister_callback(void *obj) {}

static struct bytestream_1_vt candidate_vt = {
    .read = candidate_read,
    .close = candidate_close,
    .register_callback = candidate_register_callback,
    .unregister_callback = candidate_unregister_callback
};

FSTRACE_DECL(ASYNC_TCP_CLIENT_BOGUS, "UID=%64u ADDRESS=%a ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_CLIENT_INITIATE, "UID=%64u CANDIDATE=%64u ADDRESS=%a");

static void resolved(tcp_client_t *client, list_t *addresses)
{
    assert(client->state == TCP_CLIENT_RESOLVING);
    set_client_state(client, TCP_CLIENT_CONNECTING);
    client->candidates = make_list();
    list_elem_t *e;
    for (e = list_get_first(addresses); e; e = list_next(e)) {
        socket_address_t *sa = (socket_address_t *) list_elem_get_value(e);
        tcp_conn_t *conn =
            tcp_connect(client->async, NULL, sa->address, sa->addrlen);
        if (!conn) {
            FSTRACE(ASYNC_TCP_CLIENT_BOGUS, client->uid, sa->address,
                    sa->addrlen);
            continue;
        }
        conn_candidate_t *candidate = fsalloc(sizeof *candidate);
        candidate->uid = fstrace_get_unique_id();
        FSTRACE(ASYNC_TCP_CLIENT_INITIATE, client->uid, candidate->uid,
                sa->address, sa->addrlen);
        candidate->client = client;
        candidate->conn = conn;
        bytestream_1 output_stream = { candidate, &candidate_vt };
        tcp_set_output_stream(conn, output_stream);
        list_append(client->candidates, candidate);
    }
    free_addresses(addresses);
}

static void probe_resolution(tcp_client_t *client)
{
    if (client->state == TCP_CLIENT_ZOMBIE)
        return;
    action_1_perf(client->choice_callback);
}

FSTRACE_DECL(ASYNC_TCP_CLIENT_CREATE,
             "UID=%64u PTR=%p ASYNC=%p HOST=%s PORT=%u");

tcp_client_t *open_tcp_client_2(async_t *async, const char *server_host,
                                unsigned port, fsadns_t *dns)
{
    /* Approach: start connecting to all addresses. Whichever connection
     * tries to read (or close) the output bytestream first is chosen as
     * the representative connection. */
    tcp_client_t *client = fsalloc(sizeof *client);
    client->async = async;
    client->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_TCP_CLIENT_CREATE, client->uid, client, async, server_host,
            port);
    client->choice_callback = NULL_ACTION_1;
    client->state = TCP_CLIENT_RESOLVING;
    client->port = port;
    if (!dns) {
        resolved(client, resolve_hostname(client, server_host, port));
        return client;
    }
    const struct addrinfo hint = {
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
    };
    action_1 resolution_cb = { client, (act_1) probe_resolution };
    client->query =
        fsadns_resolve(dns, server_host, NULL, &hint, resolution_cb);
    async_execute(async, resolution_cb);
    return client;
}

tcp_client_t *open_tcp_client(async_t *async, const char *server_host,
                              unsigned port)
{
    return open_tcp_client_2(async, server_host, port, NULL);
}

FSTRACE_DECL(ASYNC_TCP_CLIENT_CLOSE, "UID=%64u");

void tcp_client_close(tcp_client_t *client)
{
    FSTRACE(ASYNC_TCP_CLIENT_CLOSE, client->uid);
    tcp_client_state_t state = client->state;
    set_client_state(client, TCP_CLIENT_ZOMBIE);
    switch (state) {
        case TCP_CLIENT_RESOLVING:
            fsadns_cancel(client->query);
            break;
        case TCP_CLIENT_CONNECTING:
        case TCP_CLIENT_CONNECTED:
            while (!list_empty(client->candidates)) {
                conn_candidate_t *candidate =
                    (conn_candidate_t *) list_pop_first(client->candidates);
                tcp_close_input_stream(candidate->conn);
                /* Note: tcp_close will perform an immediate call to
                 * candidate_close(). To make this a no-op, the state
                 * must be changed to ZOMBIE before. */
                tcp_close(candidate->conn);
                async_wound(client->async, candidate);
            }
            destroy_list(client->candidates);
            break;
        case TCP_CLIENT_NOTIFIED:
            tcp_close_input_stream(client->chosen);
            tcp_close(client->chosen);
            break;
        case TCP_CLIENT_RELAYED:
            break;
        default:
            assert(false);
    }
    async_wound(client->async, client);
}

FSTRACE_DECL(ASYNC_TCP_CLIENT_RESOLVED, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_CLIENT_RESOLUTION_FAIL, "UID=%64u ERR=%I ERRNO=%e");

tcp_conn_t *resolve_and_establish(tcp_client_t *client)
{
    struct addrinfo *res;
    int err = fsadns_check(client->query, &res);
    if (err) {
        FSTRACE(ASYNC_TCP_CLIENT_RESOLUTION_FAIL, client->uid,
                trace_getaddrinfo_error, &err);
        if (err != EAI_SYSTEM)
            errno = EDESTADDRREQ;
        return NULL;
    }
    FSTRACE(ASYNC_TCP_CLIENT_RESOLVED, client->uid);
    resolved(client, parse_addrinfo(client, res, client->port));
    fsadns_freeaddrinfo(res);
    return tcp_client_establish(client);
}

FSTRACE_DECL(ASYNC_TCP_CLIENT_ESTABLISHED, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_CLIENT_ESTABLISH_FAIL, "UID=%64u ERRNO=%e");

tcp_conn_t *tcp_client_establish(tcp_client_t *client)
{
    switch (client->state) {
        case TCP_CLIENT_RESOLVING:
            return resolve_and_establish(client);
        case TCP_CLIENT_CONNECTING:
        case TCP_CLIENT_CONNECTED:
            if (list_empty(client->candidates))
                errno = EDESTADDRREQ;
            else
                errno = EAGAIN;
            FSTRACE(ASYNC_TCP_CLIENT_ESTABLISH_FAIL, client->uid);
            return NULL;
        case TCP_CLIENT_NOTIFIED:
            FSTRACE(ASYNC_TCP_CLIENT_ESTABLISHED, client->uid);
            set_client_state(client, TCP_CLIENT_RELAYED);
            return client->chosen;
        default:
            assert(false);
    }
}

FSTRACE_DECL(ASYNC_TCP_CLIENT_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void tcp_client_register_callback(tcp_client_t *client, action_1 action)
{
    FSTRACE(ASYNC_TCP_CLIENT_REGISTER, client->uid, action.obj, action.act);
    client->choice_callback = action;
}

FSTRACE_DECL(ASYNC_TCP_CLIENT_UNREGISTER, "UID=%64u");

void tcp_client_unregister_callback(tcp_client_t *client)
{
    FSTRACE(ASYNC_TCP_CLIENT_UNREGISTER, client->uid);
    client->choice_callback = NULL_ACTION_1;
}
