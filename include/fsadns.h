#pragma once

#include <netdb.h>
#include <sys/socket.h>

#include <fstrace.h>

#include "async.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fsadns fsadns_t;
typedef struct fsadns_query fsadns_query_t;

/* Make an asynchronous DNS resolver that honors Linux settings. This
 * constructor calls fork(2) internally, which means the application
 * is likely to require some administrative actions in the child
 * process. (Notably, the application will likely want to invoke
 * fstrace_reopen(). )
 */
fsadns_t *fsadns_make_resolver(async_t *async, unsigned max_parallel,
                               action_1 post_fork_cb);

/* Normally, there should be little need to destroy the resolver, but
 * this function does it and instantaneously takes away all queries
 * with it. */
void fsadns_destroy_resolver(fsadns_t *dns);

/* See getaddrinfo(3). Here, node must not be NULL. The probe callback
 * is used to suggest when would be a good time to call fsadns_check()
 * again. However, the callback is only guaranteed after
 * fsadns_check() returns with EAI_SYSTEM+EAGAIN. */
fsadns_query_t *fsadns_resolve(fsadns_t *dns, const char *node,
                               const char *service,
                               const struct addrinfo *hints, action_1 probe);

/* Collect the address resolution result. See getaddrinfo(3) for the
 * return values. In particular, EAI_SYSTEM with errno == EAGAIN is
 * returned when the result is not available yet.
 *
 * The function returns 0 when res contains a valid result.
 *
 * The function frees the query object except if it returns EAI_SYSTEM
 * with errno == EAGAIN. A freed query object must not be consulted
 * again. */
int fsadns_check(fsadns_query_t *query, struct addrinfo **res);

/* The result of fsadns_check() must be deallocated using this
 * function. */
void fsadns_freeaddrinfo(struct addrinfo *res);

/* A query can be canceled using this function. The query object is
 * freed and must not be consulted again. */
void fsadns_cancel(fsadns_query_t *query);

/* See getnameinfo(3). The probe callback is used to suggest when
 * would be a good time to call fsadns_check_name() again. However,
 * the callback is only guaranteed after fsadns_check_name() returns
 * with EAI_SYSTEM+EAGAIN. */
fsadns_query_t *fsadns_resolve_name(fsadns_t *dns, const struct sockaddr *addr,
                                    socklen_t addrlen, int flags,
                                    action_1 probe);

/* Collect the name resolution result. See getnameinfo(3) for the
 * return values. In particular, EAI_SYSTEM with errno == EAGAIN is
 * returned when the result is not available yet.
 *
 * The returned strings must be deallocated using fsfree().
 */
int fsadns_check_name(fsadns_query_t *query, char **host, char **serv);

#ifdef __cplusplus
}

#include <functional>
#include <memory>

namespace fsecure {
namespace async {

// std::unique_ptr for fsadns_t with custom deleter.
using FsadnsPtr = std::unique_ptr<fsadns_t, std::function<void(fsadns_t *)>>;

// Create FsadnsPtr that takes ownership of the provided fsadns_t. Pass
// nullptr to create an instance which doesn't contain any fsadns_t object.
inline FsadnsPtr make_fsadns_ptr(fsadns_t *adns)
{
    return { adns, fsadns_destroy_resolver };
}

} // namespace async
} // namespace fsecure

#endif
