#include "fsadns.h"

#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <fsdyn/base64.h>
#include <fsdyn/charstr.h>
#include <fsdyn/fsalloc.h>
#include <fsdyn/hashtable.h>
#include <fsdyn/list.h>
#include <fstrace.h>

#include "async_version.h"
#include "jsonthreader.h"

struct fsadns {
    async_t *async;
    uint64_t uid;
    int error; /* if non-0, the error is fatal */
    jsonthreader_t *threader;
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
        struct { /* QUERY_REPLIED_ADDRESS only */
            struct addrinfo *info;
        } address;
        struct { /* QUERY_REPLIED_NAME only */
            char *host, *serv;
        } name;
        struct { /* QUERY_ERRORED only */
            int err, err_no;
        } error;
    };
    list_elem_t *loc; /* in dns->queries */
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

static void construct_resolve_address_fail(json_thing_t *resp, int err,
                                           int syserr)
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
    if (ap->ai_addr) {
        char *base64_encoded =
            base64_encode_simple(ap->ai_addr, ap->ai_addrlen);
        json_add_to_object(info, "addr", json_adopt_string(base64_encoded));
    }
    if (ap->ai_flags & AI_CANONNAME) {
        char *url_encoded = charstr_url_encode(ap->ai_canonname);
        json_add_to_object(info, "canonname", json_adopt_string(url_encoded));
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
#ifdef EAI_NODATA
        case EAI_NODATA:
            return "EAI_NODATA";
#endif
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

static json_thing_t *resolve_address(fsadns_t *dns, json_thing_t *reqid,
                                     json_thing_t *fields)
{
    struct addrinfo hints = { .ai_flags = 0 }, *phints, *res;
    json_thing_t *hint_fields;
    if (json_object_get_object(fields, "hints", &hint_fields)) {
        phints = &hints;
        deconstruct_addrinfo(&hints, hint_fields);
        hints.ai_next = NULL;
    } else
        phints = NULL;
    json_thing_t *response = json_make_object();
    if (reqid)
        json_add_to_object(response, "reqid", json_clone(reqid));
    const char *s;
    char *node, *service;
    if (json_object_get_string(fields, "node", &s))
        node = charstr_url_decode(s, true, NULL);
    else
        assert(false);
    if (json_object_get_string(fields, "service", &s))
        service = charstr_url_decode(s, true, NULL);
    else
        service = NULL;
    FSTRACE(FSADNS_SERVE_GETADDRINFO_START, dns->uid);
    int err = getaddrinfo(node, service, phints, &res);
    if (err) {
        FSTRACE(FSADNS_SERVE_GETADDRINFO_FAIL, dns->uid,
                trace_getaddrinfo_error, &err);
        construct_resolve_address_fail(response, err, errno);
    } else {
        FSTRACE(FSADNS_SERVE_GETADDRINFO, dns->uid);
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

static json_thing_t *resolve_name(fsadns_t *dns, json_thing_t *reqid,
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
    FSTRACE(FSADNS_SERVE_GETNAMEINFO_START, dns->uid);
    int err = getnameinfo(addr, addrlen, host, sizeof host, serv, sizeof serv,
                          (int) flags);
    fsfree(addr);
    if (err) {
        FSTRACE(FSADNS_SERVE_GETNAMEINFO_FAIL, dns->uid,
                trace_getaddrinfo_error, &err);
        construct_resolve_name_fail(response, err, errno);
    } else {
        FSTRACE(FSADNS_SERVE_GETNAMEINFO, dns->uid, host, serv);
        construct_resolve_name_resp(response, host, serv);
    }
    return response;
}

FSTRACE_DECL(FSADNS_SERVE_RESOLVE, "UID=%64u PID=%P TID=%T PDU=%I");
FSTRACE_DECL(FSADNS_SERVE_RESOLVE_CONFUSED, "UID=%64u PID=%P TID=%T");

static json_thing_t *resolve(void *obj, json_thing_t *request)
{
    fsadns_t *dns = obj;
    FSTRACE(FSADNS_SERVE_RESOLVE, dns->uid, json_trace, request);
    assert(json_thing_type(request) == JSON_OBJECT);
    json_thing_t *reqid = json_object_get(request, "reqid");
    json_thing_t *fields;
    if (json_object_get_object(request, "resolve_address_req", &fields))
        return resolve_address(dns, reqid, fields);
    if (json_object_get_object(request, "resolve_name_req", &fields))
        return resolve_name(dns, reqid, fields);
    FSTRACE(FSADNS_SERVE_RESOLVE_CONFUSED, dns->uid);
    assert(false);
}

static void dns_probe(fsadns_t *dns);
static void destroy_query(fsadns_query_t *query);

FSTRACE_DECL(FSADNS_MARK_ERROR, "UID=%64u ERRNO=%E");

static void mark_protocol_error(fsadns_t *dns, int error)
{
    FSTRACE(FSADNS_MARK_ERROR, dns->uid, error);
    dns->error = error;
    list_elem_t *e = list_get_first(dns->queries);
    while (e) {
        list_elem_t *next = list_next(e);
        fsadns_query_t *q = (fsadns_query_t *) list_elem_get_value(e);
        switch (q->state) {
            case QUERY_REQUESTED_ADDRESS:
            case QUERY_REQUESTED_NAME:
            case QUERY_REPLIED_ADDRESS:
            case QUERY_REPLIED_NAME:
            case QUERY_ERRORED:
                async_execute(dns->async, q->probe);
                break;
            case QUERY_CANCELED:
                destroy_query(q);
                break;
            default:
                assert(false);
        }
        e = next;
    }
}

FSTRACE_DECL(FSADNS_CREATE, "UID=%64u PTR=%p ASYNC=%p MAX-PAR=%u");
FSTRACE_DECL(FSADNS_CREATE_JSONTHREADER_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(FSADNS_CREATE_STARTED, "UID=%64u THREADER=%p");

fsadns_t *fsadns_make_resolver(async_t *async, unsigned max_parallel,
                               action_1 post_fork_cb)
{
    fsadns_t *dns = fsalloc(sizeof *dns);
    dns->uid = fstrace_get_unique_id();
    FSTRACE(FSADNS_CREATE, dns->uid, dns, async, max_parallel);
    dns->error = 0;
    dns->async = async;
    list_t *fds_to_keep = make_list();
    list_append(fds_to_keep, as_integer(0));
    list_append(fds_to_keep, as_integer(1));
    list_append(fds_to_keep, as_integer(2));
    dns->threader = make_jsonthreader(async, fds_to_keep, post_fork_cb, resolve,
                                      dns, 100000, max_parallel);
    if (!dns->threader) {
        FSTRACE(FSADNS_CREATE_JSONTHREADER_FAIL, dns->uid);
        fsfree(dns);
        return NULL;
    }
    FSTRACE(FSADNS_CREATE_STARTED, dns->uid, dns->threader);
    jsonthreader_register_callback(dns->threader,
                                   (action_1) { dns, (act_1) dns_probe });
    async_execute(dns->async, (action_1) { dns, (act_1) dns_probe });
    dns->query_map = make_hash_table(10000, query_hash, query_cmp);
    dns->queries = make_list();
    return dns;
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
    FSTRACE(FSADNS_QUERY_SET_STATE, query->uid, trace_state, &query->state,
            trace_state, &state);
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
        default:;
    }
    list_remove(dns->queries, query->loc);
    destroy_hash_element(hash_table_pop(dns->query_map, &query->uid));
    set_query_state(query, QUERY_ZOMBIE);
    async_wound(dns->async, query);
}

FSTRACE_DECL(FSADNS_DESTROY, "UID=%64u");

void fsadns_destroy_resolver(fsadns_t *dns)
{
    FSTRACE(FSADNS_DESTROY, dns->uid);
    assert(dns->async != NULL);
    while (!list_empty(dns->queries)) {
        list_elem_t *elem = list_get_first(dns->queries);
        destroy_query((fsadns_query_t *) list_elem_get_value(elem));
    }
    destroy_list(dns->queries);
    destroy_hash_table(dns->query_map);
    jsonthreader_terminate(dns->threader);
    destroy_jsonthreader(dns->threader);
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

fsadns_query_t *fsadns_resolve(fsadns_t *dns, const char *node,
                               const char *service,
                               const struct addrinfo *hints, action_1 probe)
{
    if (dns->error) {
        errno = dns->error;
        return NULL;
    }
    fsadns_query_t *query = make_address_query(dns, probe);
    FSTRACE(FSADNS_ADDRESS_QUERY_CREATE, query->uid, query, dns->uid, node,
            service);
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
        FSTRACE(FSADNS_ADDRESS_QUERY_HINTS, query->uid, hints->ai_flags,
                hints->ai_family, hints->ai_socktype, hints->ai_protocol,
                hints->ai_addr, hints->ai_addrlen,
                hints->ai_flags & AI_CANONNAME ? hints->ai_canonname : NULL);
        json_add_to_object(fields, "hints", construct_addrinfo(hints));
    } else
        FSTRACE(FSADNS_ADDRESS_QUERY_NO_HINTS, query->uid);
    jsonthreader_send(dns->threader, request);
    json_destroy_thing(request);
    return query;
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
    else
        *err_no = -1;
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
    else
        *err_no = -1;
    FSTRACE(FSADNS_GOOD_NAME_FAILURE, dns->uid, *error, *err_no);
    return true;
}

FSTRACE_DECL(FSADNS_UNEXPECTED_REQID, "UID=%64u REQID=%64u");
FSTRACE_DECL(FSADNS_RELAY_ADDRESS_RESPONSE, "UID=%64u REQID=%64u");
FSTRACE_DECL(FSADNS_RELAY_NAME_RESPONSE, "UID=%64u REQID=%64u");
FSTRACE_DECL(FSADNS_CLEAR_CANCELED, "UID=%64u REQID=%64u");
FSTRACE_DECL(FSADNS_QUERY_CONFUSED, "UID=%64u");

static bool relay_response(fsadns_t *dns, json_thing_t *response,
                           uint64_t reqid)
{
    hash_elem_t *e = hash_table_get(dns->query_map, &reqid);
    if (!e) {
        FSTRACE(FSADNS_UNEXPECTED_REQID, dns->uid, reqid);
        return false;
    }
    fsadns_query_t *query = (fsadns_query_t *) hash_elem_get_value(e);
    switch (query->state) {
        case QUERY_REQUESTED_ADDRESS:
            FSTRACE(FSADNS_RELAY_ADDRESS_RESPONSE, dns->uid, query->uid);
            {
                struct addrinfo *info;
                if (parse_address_response(dns, response, &info)) {
                    set_query_state(query, QUERY_REPLIED_ADDRESS);
                    query->address.info = info;
                    async_execute(dns->async, query->probe);
                    return true;
                }
                int err, err_no;
                if (parse_address_failure(dns, response, &err, &err_no)) {
                    set_query_state(query, QUERY_ERRORED);
                    query->error.err = err;
                    query->error.err_no = err_no;
                    async_execute(dns->async, query->probe);
                    return true;
                }
            }
            FSTRACE(FSADNS_DECODE_ERROR, dns->uid);
            return false;
        case QUERY_REQUESTED_NAME:
            FSTRACE(FSADNS_RELAY_NAME_RESPONSE, dns->uid, query->uid);
            {
                char *host, *serv;
                if (parse_name_response(dns, response, &host, &serv)) {
                    set_query_state(query, QUERY_REPLIED_NAME);
                    query->name.host = host;
                    query->name.serv = serv;
                    async_execute(dns->async, query->probe);
                    return true;
                }
                int err, err_no;
                if (parse_name_failure(dns, response, &err, &err_no)) {
                    set_query_state(query, QUERY_ERRORED);
                    query->error.err = err;
                    query->error.err_no = err_no;
                    async_execute(dns->async, query->probe);
                    return true;
                }
            }
            FSTRACE(FSADNS_DECODE_ERROR, dns->uid);
            return false;
        case QUERY_CANCELED:
            FSTRACE(FSADNS_CLEAR_CANCELED, dns->uid, query->uid);
            destroy_query(query);
            return true;
        default:
            FSTRACE(FSADNS_QUERY_CONFUSED, query->uid);
            return false;
    }
}

FSTRACE_DECL(FSADNS_DNS_PROBE_POSTHUMOUS, "UID=%64u");
FSTRACE_DECL(FSADNS_DNS_PROBE, "UID=%64u");
FSTRACE_DECL(FSADNS_DNS_PROBE_RECV_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(FSADNS_DNS_PROBE_RECV, "UID=%64u PDU=%I");

static void dns_probe(fsadns_t *dns)
{
    if (!dns->async) {
        FSTRACE(FSADNS_DNS_PROBE_POSTHUMOUS, dns->uid);
        return;
    }
    FSTRACE(FSADNS_DNS_PROBE, dns->uid);
    for (;;) {
        json_thing_t *response = jsonthreader_receive(dns->threader);
        if (!response) {
            FSTRACE(FSADNS_DNS_PROBE_RECV_FAIL, dns->uid);
            switch (errno) {
                case 0:
                    mark_protocol_error(dns, EPROTO);
                    return;
                case EAGAIN:
                    return;
                default:
                    mark_protocol_error(dns, errno);
                    return;
            }
        }
        FSTRACE(FSADNS_DNS_PROBE_RECV, dns->uid, json_trace, response);
        uint64_t reqid;
        if (!get_response_reqid(dns, response, &reqid) ||
            !relay_response(dns, response, reqid)) {
            json_destroy_thing(response);
            mark_protocol_error(dns, EPROTO);
            return;
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
            errno = EAGAIN;
            return EAI_SYSTEM;
        case QUERY_REPLIED_ADDRESS:
            FSTRACE(FSADNS_ADDRESS_QUERY_REPLIED, query->uid,
                    query->address.info);
            *res = query->address.info;
            set_query_state(query, QUERY_CONSUMED);
            destroy_query(query);
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

fsadns_query_t *fsadns_resolve_name(fsadns_t *dns, const struct sockaddr *addr,
                                    socklen_t addrlen, int flags,
                                    action_1 probe)
{
    if (dns->error) {
        errno = dns->error;
        return NULL;
    }
    fsadns_query_t *query = make_name_query(dns, probe);
    FSTRACE(FSADNS_NAME_QUERY_CREATE, query->uid, query, dns->uid, addr,
            addrlen);
    json_thing_t *request = json_make_object();
    json_add_to_object(request, "reqid", json_make_unsigned(query->uid));
    json_thing_t *fields = json_make_object();
    json_add_to_object(request, "resolve_name_req", fields);
    char *base64_encoded = base64_encode_simple(addr, addrlen);
    json_add_to_object(fields, "addr", json_adopt_string(base64_encoded));
    json_add_to_object(fields, "flags", json_make_integer(flags));
    jsonthreader_send(dns->threader, request);
    json_destroy_thing(request);
    return query;
}

FSTRACE_DECL(FSADNS_NAME_QUERY_REPLIED, "UID=%64u HOST=%s SERV=%s");
FSTRACE_DECL(FSADNS_NAME_QUERY_ERRORED, "UID=%64u ERRNO=%E");
FSTRACE_DECL(FSADNS_POSTHUMOUS_NAME_CHECK, "UID=%64u");

int fsadns_check_name(fsadns_query_t *query, char **host, char **serv)
{
    fsadns_t *dns = query->dns;
    switch (query->state) {
        case QUERY_REQUESTED_NAME:
            errno = EAGAIN;
            return EAI_SYSTEM;
        case QUERY_REPLIED_NAME:
            FSTRACE(FSADNS_NAME_QUERY_REPLIED, query->uid, query->name.host,
                    query->name.serv);
            *host = query->name.host;
            *serv = query->name.serv;
            set_query_state(query, QUERY_CONSUMED);
            destroy_query(query);
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
