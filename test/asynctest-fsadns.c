#include <errno.h>
#include <netinet/in.h>
#include <string.h>

#include <async/async.h>
#include <async/fsadns.h>
#include <fsdyn/fsalloc.h>
#include <fsdyn/list.h>

#include "asynctest.h"

typedef struct {
    tester_base_t base;
    fsadns_t *dns;
    list_t *queries;
} global_t;

typedef struct {
    fsadns_query_t *fq;
    const char *address;
    list_elem_t *loc;
    global_t *g;
} query_t;

static void dump_address(const struct addrinfo *res)
{
    if (res->ai_addrlen >= sizeof(struct sockaddr))
        switch (res->ai_addr->sa_family) {
            case AF_INET: {
                const struct sockaddr_in *ipv4 =
                    (const struct sockaddr_in *) res->ai_addr;
                uint32_t quad = ntohl(ipv4->sin_addr.s_addr);
                uint16_t port = ntohs(ipv4->sin_port);
                char buf[64];
                snprintf(buf, sizeof buf, "%d.%d.%d.%d", quad >> 24 & 0xff,
                         quad >> 16 & 0xff, quad >> 8 & 0xff, quad & 0xff);
                if (port)
                    tlog("  addr (IPv4) = %s/%d", buf, port);
                else
                    tlog("  addr (IPv4) = %s", buf);
                break;
            }
            case AF_INET6: {
                const struct sockaddr_in6 *ipv6 =
                    (const struct sockaddr_in6 *) res->ai_addr;
                const uint16_t *a = (const uint16_t *) ipv6->sin6_addr.s6_addr;
                uint16_t port = ntohs(ipv6->sin6_port);
                char buf[64];
                snprintf(buf, sizeof buf, "%x:%x:%x:%x:%x:%x:%x:%x",
                         ntohs(a[0]), ntohs(a[1]), ntohs(a[2]), ntohs(a[3]),
                         ntohs(a[4]), ntohs(a[5]), ntohs(a[6]), ntohs(a[7]));
                if (port)
                    tlog("  addr (IPv6) = %s/%d", buf, port);
                else
                    tlog("  addr (IPv6) = %s", buf);
                break;
            }
            default: {
                tlog("  addr (%d) = ?", res->ai_addr->sa_family);
            }
        }
    else {
        tlog("  addr (?) = ?");
    }
}

static void dump_protocol(int protocol)
{
    switch (protocol) {
        case IPPROTO_IP:
            tlog("  protocol = IP");
            break;
        case IPPROTO_ICMP:
            tlog("  protocol = ICMP");
            break;
        case IPPROTO_IGMP:
            tlog("  protocol = IGMP");
            break;
        case IPPROTO_IPIP:
            tlog("  protocol = IPIP");
            break;
        case IPPROTO_TCP:
            tlog("  protocol = TCP");
            break;
        case IPPROTO_EGP:
            tlog("  protocol = EGP");
            break;
        case IPPROTO_PUP:
            tlog("  protocol = PUP");
            break;
        case IPPROTO_UDP:
            tlog("  protocol = UDP");
            break;
        case IPPROTO_IDP:
            tlog("  protocol = IDP");
            break;
        case IPPROTO_TP:
            tlog("  protocol = TP");
            break;
        case IPPROTO_IPV6:
            tlog("  protocol = IPV6");
            break;
        case IPPROTO_RSVP:
            tlog("  protocol = RSVP");
            break;
        case IPPROTO_GRE:
            tlog("  protocol = GRE");
            break;
        case IPPROTO_ESP:
            tlog("  protocol = ESP");
            break;
        case IPPROTO_AH:
            tlog("  protocol = AH");
            break;
#ifdef IPPROTO_MTP
        case IPPROTO_MTP:
            tlog("  protocol = MTP");
            break;
#endif
        case IPPROTO_ENCAP:
            tlog("  protocol = ENCAP");
            break;
        case IPPROTO_PIM:
            tlog("  protocol = PIM");
            break;
#ifdef IPPROTO_SCTP
        case IPPROTO_SCTP:
            tlog("  protocol = SCTP");
            break;
#endif
        default:
            tlog("  protocol = %d", protocol);
    }
}

static void dump_query_result(const char *hostname, const struct addrinfo *res)
{
    tlog("%s resolved:", hostname);
    int i;
    for (i = 0; res; res = res->ai_next, i++) {
        tlog("  %d.", i);
        if (res->ai_flags) {
            tlog("  flags = 0x%x", res->ai_flags);
            if (res->ai_flags & AI_ADDRCONFIG)
                tlog("    AI_ADDRCONFIG");
#ifdef AI_ALL
            if (res->ai_flags & AI_ALL)
                tlog("    AI_ALL");
#endif
            if (res->ai_flags & AI_CANONNAME)
                tlog("    AI_CANONNAME");
            if (res->ai_flags & AI_NUMERICHOST)
                tlog("    AI_NUMERICHOST");
            if (res->ai_flags & AI_NUMERICSERV)
                tlog("    AI_NUMERICSERV");
            if (res->ai_flags & AI_PASSIVE)
                tlog("    AI_PASSIVE");
#ifdef AI_V4MAPPED
            if (res->ai_flags & AI_V4MAPPED)
                tlog("    AI_V4MAPPED");
#endif
        }
        switch (res->ai_family) {
            case AF_INET:
                tlog("  family = AF_INET");
                break;
            case AF_INET6:
                tlog("  family = AF_INET6");
                break;
            case AF_UNSPEC:
                tlog("  family = AF_UNSPEC");
                break;
            default:
                tlog("  family = %u", res->ai_family);
        }
        switch (res->ai_socktype) {
            case 0:
                break;
            case SOCK_STREAM:
                tlog("  socktype = SOCK_STREAM");
                break;
            case SOCK_DGRAM:
                tlog("  socktype = SOCK_DGRAM");
                break;
            default:
                tlog("  socktype = %d", res->ai_socktype);
        }
        dump_protocol(res->ai_protocol);
        if (res->ai_addrlen)
            dump_address(res);
        if (res->ai_flags & AI_CANONNAME)
            tlog("  canonname = \"%s\"", res->ai_canonname);
        tlog_string("");
    }
}

static void dump_query_failure(const char *hostname, int err)
{
    if (err == EAI_SYSTEM)
        tlog("%s failed to resolve; error = %s", hostname, strerror(errno));
    else
        tlog("%s failed to resolve; error = %s", hostname, gai_strerror(err));
    tlog_string("");
}

static void probe_query(query_t *query)
{
    if (!query->g->base.async)
        return;

    struct addrinfo *res;
    int err = fsadns_check(query->fq, &res);
    switch (err) {
        case 0:
            dump_query_result(query->address, res);
            fsadns_freeaddrinfo(res);
            break;
        case EAI_SYSTEM:
            if (errno == EAGAIN)
                return;
            dump_query_failure(query->address, errno);
            quit_test(&query->g->base);
            return;
#ifdef EAI_NODATA
        case EAI_NODATA:
#endif
        case EAI_NONAME:
            dump_query_failure(query->address, err);
            break;
        default:
            dump_query_failure(query->address, errno);
            quit_test(&query->g->base);
            return;
    }
    list_remove(query->g->queries, query->loc);
    if (list_empty(query->g->queries)) {
        quit_test(&query->g->base);
    }
    fsfree(query);
}

static void make_query(global_t *g, const char *hostname)
{
    query_t *query = fsalloc(sizeof *query);
    query->g = g;
    action_1 callback = { query, (act_1) probe_query };
    query->address = hostname;
    query->fq = fsadns_resolve(g->dns, hostname, NULL, NULL, callback);
    async_execute(g->base.async, callback);
    query->loc = list_append(g->queries, query);
}

static void probe_name_query(query_t *query)
{
    if (!query->g->base.async)
        return;

    char *host;
    char *serv;
    int err = fsadns_check_name(query->fq, &host, &serv);
    switch (err) {
        case 0:
            break;
        case EAI_SYSTEM:
            if (errno == EAGAIN)
                return;
            dump_query_failure(query->address, errno);
            quit_test(&query->g->base);
            return;
        default:
            dump_query_failure(query->address, err);
            quit_test(&query->g->base);
            return;
    }
    tlog("%s resolved; host = %s, serv = %s", query->address, host, serv);
    tlog_string("");
    list_remove(query->g->queries, query->loc);
    if (list_empty(query->g->queries)) {
        quit_test(&query->g->base);
    }
    fsfree(host);
    fsfree(serv);
    fsfree(query);
}

static void make_name_query(global_t *g, const char *address,
                            const struct sockaddr *addr, socklen_t addrlen)
{
    query_t *query = fsalloc(sizeof *query);
    query->g = g;
    action_1 callback = { query, (act_1) probe_name_query };
    query->address = address;
    query->fq = fsadns_resolve_name(g->dns, addr, addrlen, 0, callback);
    async_execute(g->base.async, callback);
    query->loc = list_append(g->queries, query);
}

static void kick_off(global_t *g)
{
    make_query(g, "f-secure.com");
    make_query(g, "www.f-secure.fi");
    make_query(g, "google.com");
    make_query(g, "example.com");
    make_query(g, "none.f-secure.com");
    struct sockaddr_in gdns_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr = { .s_addr = htonl(0x08080808) },
    };
    make_name_query(g, "8.8.8.8", (struct sockaddr *) &gdns_addr,
                    sizeof gdns_addr);
    struct sockaddr_in local_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(0),
        .sin_addr = { .s_addr = htonl(0x7f000001) },
    };
    make_name_query(g, "127.0.0.1", (struct sockaddr *) &local_addr,
                    sizeof local_addr);
    struct sockaddr_in bc_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(0),
        .sin_addr = { .s_addr = htonl(0xffffffff) },
    };
    make_name_query(g, "255.255.255.255", (struct sockaddr *) &bc_addr,
                    sizeof bc_addr);
}

VERDICT test_fsadns(void)
{
    async_t *async = make_async();
    action_1 post_fork_cb = { NULL, (act_1) reinit_trace };
    global_t g = {
        .dns = fsadns_make_resolver(async, 10, post_fork_cb),
        .queries = make_list(),
    };
    init_test(&g.base, async, 60);
    kick_off(&g);
    while (async_loop(async) < 0)
        if (errno != EINTR) {
            perror("fsadns_test");
            return EXIT_FAILURE;
        }
    fsadns_destroy_resolver(g.dns);
    async_flush(async, async_now(async) + 5 * ASYNC_S);
    destroy_async(async);
    g.base.verdict = list_empty(g.queries) ? PASS : FAIL;
    destroy_list(g.queries);
    return posttest_check(g.base.verdict);
}
