#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <pthread.h>
#include <fsdyn/fsalloc.h>
#include <fsdyn/list.h>
#include <async/async.h>
#include <fstrace.h>
#include <async/fsadns.h>

typedef struct {
    async_t *async;
    fsadns_t *dns;
    list_t *queries;
} global_t;

typedef struct {
    fsadns_query_t *fq;
    const char *hostname;
    list_elem_t *loc;
    global_t *g;
} query_t;

typedef struct {
    fsadns_query_t *fq;
    list_elem_t *loc;
    global_t *g;
} name_query_t;

static void dump_address(const struct addrinfo *res)
{
    if (res->ai_addrlen >= sizeof(struct sockaddr))
        switch (res->ai_addr->sa_family) {
            case AF_INET: {
                const struct sockaddr_in *ipv4 =
                    (const struct sockaddr_in *) res->ai_addr;
                uint32_t quad = ntohl(ipv4->sin_addr.s_addr);
                uint16_t port = ntohs(ipv4->sin_port);
                printf("  addr (IPv4) = %d.%d.%d.%d",
                       quad >> 24 & 0xff, quad >> 16 & 0xff,
                       quad >> 8 & 0xff, quad & 0xff);
                if (port)
                    printf("/%d\n", port);
                else printf("\n");
                break;
            }
            case AF_INET6: {
                const struct sockaddr_in6 *ipv6 =
                    (const struct sockaddr_in6 *) res->ai_addr;
                const uint16_t *a =
                    (const uint16_t *) ipv6->sin6_addr.s6_addr;
                uint16_t port = ntohs(ipv6->sin6_port);
                printf("  addr (IPv6) = %x:%x:%x:%x:%x:%x:%x:%x",
                       ntohs(a[0]), ntohs(a[1]),
                       ntohs(a[2]), ntohs(a[3]),
                       ntohs(a[4]), ntohs(a[5]),
                       ntohs(a[6]), ntohs(a[7]));
                if (port)
                    printf("/%d\n", port);
                else printf("\n");
                break;
            }
            default: {
                printf("  addr (%d) = ", res->ai_addr->sa_family);
                socklen_t i;
                for (i = 0; i < res->ai_addrlen; i++)
                    printf("%02x", ((uint8_t *) res->ai_addr)[i]);
                printf("\n");
            }
        }
    else {
        printf("  addr (?) = ");
        socklen_t i;
        for (i = 0; i < res->ai_addrlen; i++)
            printf("%02x", ((uint8_t *) res->ai_addr)[i]);
        printf("\n");
    }
}

static void dump_protocol(int protocol)
{
    switch (protocol) {
        case IPPROTO_IP:
            printf("  protocol = IP\n");
            break;
        case IPPROTO_ICMP:
            printf("  protocol = ICMP\n");
            break;
        case IPPROTO_IGMP:
            printf("  protocol = IGMP\n");
            break;
        case IPPROTO_IPIP:
            printf("  protocol = IPIP\n");
            break;
        case IPPROTO_TCP:
            printf("  protocol = TCP\n");
            break;
        case IPPROTO_EGP:
            printf("  protocol = EGP\n");
            break;
        case IPPROTO_PUP:
            printf("  protocol = PUP\n");
            break;
        case IPPROTO_UDP:
            printf("  protocol = UDP\n");
            break;
        case IPPROTO_IDP:
            printf("  protocol = IDP\n");
            break;
        case IPPROTO_TP:
            printf("  protocol = TP\n");
            break;
        case IPPROTO_IPV6:
            printf("  protocol = IPV6\n");
            break;
        case IPPROTO_RSVP:
            printf("  protocol = RSVP\n");
            break;
        case IPPROTO_GRE:
            printf("  protocol = GRE\n");
            break;
        case IPPROTO_ESP:
            printf("  protocol = ESP\n");
            break;
        case IPPROTO_AH:
            printf("  protocol = AH\n");
            break;
        case IPPROTO_MTP:
            printf("  protocol = MTP\n");
            break;
        case IPPROTO_ENCAP:
            printf("  protocol = ENCAP\n");
            break;
        case IPPROTO_PIM:
            printf("  protocol = PIM\n");
            break;
        case IPPROTO_SCTP:
            printf("  protocol = SCTP\n");
            break;
        default:
            printf("  protocol = %d\n", protocol);
    }
}

static void dump_query_result(const char *hostname, const struct addrinfo *res)
{
    printf("%s resolved:\n", hostname);
    int i;
    for (i = 0; res; res = res->ai_next, i++) {
        printf("  %d.\n", i);
        if (res->ai_flags) {
            printf("  flags = 0x%x\n", res->ai_flags);
            if (res->ai_flags & AI_ADDRCONFIG)
                printf("    AI_ADDRCONFIG\n");
            if (res->ai_flags & AI_ALL)
                printf("    AI_ALL\n");
            if (res->ai_flags & AI_CANONNAME)
                printf("    AI_CANONNAME\n");
            if (res->ai_flags & AI_NUMERICHOST)
                printf("    AI_NUMERICHOST\n");
            if (res->ai_flags & AI_NUMERICSERV)
                printf("    AI_NUMERICSERV\n");
            if (res->ai_flags & AI_PASSIVE)
                printf("    AI_PASSIVE\n");
            if (res->ai_flags & AI_V4MAPPED)
                printf("    AI_V4MAPPED\n");
        }
        switch (res->ai_family) {
            case AF_INET:
                printf("  family = AF_INET\n");
                break;
            case AF_INET6:
                printf("  family = AF_INET6\n");
                break;
            case AF_UNSPEC:
                printf("  family = AF_UNSPEC\n");
                break;
            default:
                printf("  family = %u\n", res->ai_family);
        }
        switch (res->ai_socktype) {
            case 0:
                break;
            case SOCK_STREAM:
                printf("  socktype = SOCK_STREAM\n");
                break;
            case SOCK_DGRAM:
                printf("  socktype = SOCK_DGRAM\n");
                break;
            default:
                printf("  socktype = %d\n", res->ai_socktype);
        }
        dump_protocol(res->ai_protocol);
        if (res->ai_addrlen)
            dump_address(res);
        if (res->ai_flags & AI_CANONNAME)
            printf("  canonname = \"%s\"\n", res->ai_canonname);
        printf("\n");
    }
}

static void dump_query_failure(const char *hostname, int err)
{
    if (err == EAI_SYSTEM)
        printf("%s failed to resolve; err = EAI_SYSTEM, errno = %d\n\n",
               hostname, errno);
    else printf("%s failed to resolve; err = %d\n\n",
                hostname, err);
}

static void probe_query(query_t *query)
{
    struct addrinfo *res;
    int err = fsadns_check(query->fq, &res);
    switch (err) {
        case 0:
            dump_query_result(query->hostname, res);
            fsadns_freeaddrinfo(res);
            break;
        case EAI_SYSTEM:
            if (errno == EAGAIN)
                return;
            fprintf(stderr, "Got error: EAI_SYSTEM (errno = %d)\n", errno);
            assert(false);
            break;
        case EAI_NONAME:
            dump_query_failure(query->hostname, err);
            break;
        default:
            fprintf(stderr, "Got error: %d\n", err);
            assert(false);
    }
    list_remove(query->g->queries, query->loc);
    if (list_empty(query->g->queries)) {
        printf("Done!\n");
        async_quit_loop(query->g->async);
    }
    fsfree(query);
}

static void make_query(global_t *g, const char *hostname)
{
    query_t *query = fsalloc(sizeof *query);
    query->g = g;
    action_1 callback = { query, (act_1) probe_query };
    query->hostname = hostname;
    query->fq = fsadns_resolve(g->dns, hostname, NULL, NULL, callback);
    async_execute(g->async, callback);
    query->loc = list_append(g->queries, query);
}

static void probe_name_query(name_query_t *query)
{
    char *host;
    char *serv;
    int err = fsadns_check_name(query->fq, &host, &serv);
    switch (err) {
        case 0:
            break;
        case EAI_SYSTEM:
            if (errno == EAGAIN)
                return;
            fprintf(stderr, "Got error: EAI_SYSTEM (errno = %d)\n", errno);
            assert(false);
            break;
        default:
            fprintf(stderr, "Got error: %d\n", err);
            assert(false);
    }
    printf("Got name: host = %s, serv = %s\n\n", host, serv);
    list_remove(query->g->queries, query->loc);
    if (list_empty(query->g->queries)) {
        printf("Done!\n");
        async_quit_loop(query->g->async);
    }
    fsfree(host);
    fsfree(serv);
    fsfree(query);
}

static void make_name_query(global_t *g, const struct sockaddr *addr,
                            socklen_t addrlen)
{
    name_query_t *query = fsalloc(sizeof *query);
    query->g = g;
    action_1 callback = { query, (act_1) probe_name_query };
    query->fq = fsadns_resolve_name(g->dns, addr, addrlen, 0, callback);
    async_execute(g->async, callback);
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
        .sin_addr = {
            .s_addr = htonl(0x08080808)
        }
    };
    make_name_query(g, (struct sockaddr *) &gdns_addr, sizeof gdns_addr);
    struct sockaddr_in local_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(0),
        .sin_addr = {
            .s_addr = htonl(0x7f000001)
        }
    };
    make_name_query(g, (struct sockaddr *) &local_addr, sizeof local_addr);
    struct sockaddr_in bc_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(0),
        .sin_addr = {
            .s_addr = htonl(0xffffffff)
        }
    };
    make_name_query(g, (struct sockaddr *) &bc_addr, sizeof bc_addr);
}

int main()
{
    fstrace_t *trace = fstrace_open("/tmp/fsadns_test", -1);
    fstrace_declare_globals(trace);
    fstrace_select_regex(trace, ".", NULL);
    async_t *async = make_async();
    action_1 post_fork_cb = { trace, (act_1) (void *) fstrace_reopen };
    global_t g = {
        .async = async,
        .dns = fsadns_make_resolver(async, 10, post_fork_cb),
        .queries = make_list(),
    };
    kick_off(&g);
    while (async_loop(async) < 0)
        if (errno != EINTR) {
            perror("fsadns_test");
            return EXIT_FAILURE;
        }
    fsadns_destroy_resolver(g.dns);
    destroy_list(g.queries);
    async_flush(async, async_now(async) + 5 * ASYNC_S);
    destroy_async(async);
    fstrace_close(trace);
    return EXIT_SUCCESS;
}
