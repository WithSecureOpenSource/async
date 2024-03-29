#include "tcp_connection.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

#include <fsdyn/fsalloc.h>
#include <fsdyn/integer.h>
#include <fsdyn/list.h>
#include <fstrace.h>

#include "async.h"
#include "async_version.h"
#include "drystream.h"

enum {
    OUTBUF_SIZE = 1024 * 10,
    CMSG_ASYNC_MAX_FD = 100, /* <= SCM_MAX_FD */
};

struct tcp_conn {
    async_t *async;
    uint64_t uid;
    bool connection_closed, input_stream_closed;
    struct {
        int state, error;
        uint64_t byte_count;
        list_t *ancillary_list; /* of ancillary_data_t */
    } input, output;
    action_1 notify_input;
    void (*flush_socket)(tcp_conn_t *);
    int fd;
    bytestream_1 output_stream;
    uint8_t outbuf[OUTBUF_SIZE];
    int outcursor, outcount;
    uint32_t flags;
};

struct tcp_server {
    async_t *async;
    uint64_t uid;
    int fd;
    action_1 notify;
};

typedef enum {
    ANCILLARY_RAW = 0,
    ANCILLARY_FD = 1,
    ANCILLARY_ACTION = 2,
} ancillary_kind_t;

typedef struct {
    ancillary_kind_t kind;
    union {
        struct cmsghdr *raw;
        int fd;
        action_1 action;
    };
} ancillary_data_t;

/*
 * The states. For input and output independently.
 *
 * The legal input_state/output_state combinations:
 *  - if one is CONNECTING, the other is CONNECTING or SHUT_DOWN
 *  - if one is CONNECTED, the other is CONNECTED, ENDED or SHUT_DOWN
 *  - if one is ENDED, the other is CONNECTED, ENDED or SHUT_DOWN
 *  - if one is SHUT_DOWN, the other is CONNECTING, CONNECTED, ENDED or
 *    SHUTDOWN
 *
 * CONNECTING indicates the connection has not yet been established so
 * I/O is not possible yet.
 *
 * CONNECTED indicates I/O is possible.
 *
 * ENDED indicates and EOF or error has been encountered and further I/O
 * is not possible.
 *
 * SHUT_DOWN indicates the I/O direction has been shut down by the user
 * with tcp_shut_down().
 */
enum {
    CONNECTING,
    CONNECTED,
    ENDED,
    SHUT_DOWN,
};

static bool inactive(tcp_conn_t *conn)
{
    return conn->connection_closed;
}

static size_t ancillary_data_size(const struct cmsghdr *cp)
{
    /* Linux doesn't provide an appropriate macro. */
    return cp->cmsg_len - (size_t)(uintptr_t) CMSG_DATA((struct cmsghdr *) 0);
}

static size_t ancillary_data_info(struct cmsghdr *cp, int *level, int *type)
{
    *level = cp->cmsg_level;
    *type = cp->cmsg_type;
    return ancillary_data_size(cp);
}

ancillary_data_t *make_raw_ancillary(struct cmsghdr *raw)
{
    ancillary_data_t *data = fsalloc(sizeof *data);
    data->kind = ANCILLARY_RAW;
    data->raw = raw;
    return data;
}

ancillary_data_t *make_fd_ancillary(int fd)
{
    ancillary_data_t *data = fsalloc(sizeof *data);
    data->kind = ANCILLARY_FD;
    data->fd = fd;
    return data;
}

ancillary_data_t *make_action_ancillary(action_1 action)
{
    ancillary_data_t *data = fsalloc(sizeof *data);
    data->kind = ANCILLARY_ACTION;
    data->action = action;
    return data;
}

static void destroy_ancillary_data(ancillary_data_t *data)
{
    if (data->kind == ANCILLARY_RAW)
        fsfree(data->raw);
    fsfree(data);
}

FSTRACE_DECL(ASYNC_TCP_RECEIVE_ANCILLARY, "UID=%64u SIZE=%z");
FSTRACE_DECL(ASYNC_TCP_RECEIVE_ANCILLARY_DUMP, "UID=%64u DATA=%A");

static void receive_ancillary_data(tcp_conn_t *conn, struct cmsghdr *cp)
{
    size_t anclen = cp->cmsg_len;
    FSTRACE(ASYNC_TCP_RECEIVE_ANCILLARY, conn->uid, anclen);
    struct cmsghdr *anc = fsalloc(anclen);
    memcpy(anc, cp, anclen);
    ancillary_data_t *data = make_raw_ancillary(anc);
    list_append(conn->input.ancillary_list, data);
    FSTRACE(ASYNC_TCP_RECEIVE_ANCILLARY_DUMP, conn->uid, anc, anclen);
}

static ssize_t receive(tcp_conn_t *conn, void *buf, size_t size)
{
    if (!size)
        return 0; /* recvmsg() doesn't do this */
    struct iovec iov = {
        .iov_base = buf,
        .iov_len = size,
    };
    uint8_t ancillary[1024]; /* enough? */
    struct msghdr message = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ancillary,
        .msg_controllen = sizeof ancillary,
    };
    ssize_t count = recvmsg(conn->fd, &message, 0);
    if (count < 0)
        return count;
    struct cmsghdr *cp;
    for (cp = CMSG_FIRSTHDR(&message); cp; cp = CMSG_NXTHDR(&message, cp)) {
        receive_ancillary_data(conn, cp);
    }
    return count;
}

FSTRACE_DECL(ASYNC_TCP_READ_INACTIVE, "UID=%64u WANT=%z");
FSTRACE_DECL(ASYNC_TCP_READ, "UID=%64u WANT=%z GOT=%z");
FSTRACE_DECL(ASYNC_TCP_READ_DUMP, "UID=%64u DATA=%A");
FSTRACE_DECL(ASYNC_TCP_READ_EOF, "UID=%64u WANT=%z");
FSTRACE_DECL(ASYNC_TCP_READ_FAILED, "UID=%64u WANT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_READ_CONNECTING, "UID=%64u WANT=%z");
FSTRACE_DECL(ASYNC_TCP_READ_ENDED, "UID=%64u WANT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_READ_SHUT_DOWN, "UID=%64u WANT=%z ERRNO=%e");

ssize_t tcp_read(tcp_conn_t *conn, void *buf, size_t count)
{
    if (inactive(conn)) {
        FSTRACE(ASYNC_TCP_READ_INACTIVE, conn->uid, count);
        errno = EBADF;
        return -1;
    }
    conn->flags &= ~TCP_FLAG_INGRESS_PENDING;
    ssize_t n;
    switch (conn->input.state) {
        case CONNECTED:
            n = receive(conn, buf, count);
            if (n > 0) {
                FSTRACE(ASYNC_TCP_READ, conn->uid, count, n);
                FSTRACE(ASYNC_TCP_READ_DUMP, conn->uid, buf, n);
                conn->input.byte_count += n;
            } else if (n == 0)
                FSTRACE(ASYNC_TCP_READ_EOF, conn->uid, count);
            else {
                FSTRACE(ASYNC_TCP_READ_FAILED, conn->uid, count);
                if (errno == EAGAIN)
                    conn->flags |= TCP_FLAG_EPOLL_RECV;
            }
            return n;
        case CONNECTING:
            FSTRACE(ASYNC_TCP_READ_CONNECTING, conn->uid, count);
            errno = EAGAIN;
            return -1;
        case ENDED:
            errno = conn->input.error;
            FSTRACE(ASYNC_TCP_READ_ENDED, conn->uid, count);
            return -1;
        case SHUT_DOWN:
            errno = conn->input.error;
            FSTRACE(ASYNC_TCP_READ_SHUT_DOWN, conn->uid, count);
            return -1;
        default:
            abort();
    }
}

static ssize_t _read(void *obj, void *buf, size_t count)
{
    return tcp_read(obj, buf, count);
}

void set_output_stream(tcp_conn_t *conn, bytestream_1 output_stream);

FSTRACE_DECL(ASYNC_TCP_RESET_OUTPUT, "UID=%64u");

static void reset_output_stream(tcp_conn_t *conn)
{
    FSTRACE(ASYNC_TCP_RESET_OUTPUT, conn->uid);
    set_output_stream(conn, drystream);
}

static const char *trace_state(void *p)
{
    switch (*(int *) p) {
        case CONNECTING:
            return "CONNECTING";
        case CONNECTED:
            return "CONNECTED";
        case ENDED:
            return "ENDED";
        case SHUT_DOWN:
            return "SHUT_DOWN";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNC_TCP_SET_INPUT_STATE, "UID=%64u OLD=%I NEW=%I");

static void set_input_state(tcp_conn_t *conn, int state)
{
    FSTRACE(ASYNC_TCP_SET_INPUT_STATE, conn->uid, trace_state,
            &conn->input.state, trace_state, &state);
    conn->input.state = state;
}

FSTRACE_DECL(ASYNC_TCP_SET_OUTPUT_STATE, "UID=%64u OLD=%I NEW=%I");

static void set_output_state(tcp_conn_t *conn, int state)
{
    FSTRACE(ASYNC_TCP_SET_OUTPUT_STATE, conn->uid, trace_state,
            &conn->output.state, trace_state, &state);
    conn->output.state = state;
}

FSTRACE_DECL(ASYNC_TCP_SHUT_DOWN_INACTIVE, "UID=%64u HOW=0x%x");
FSTRACE_DECL(ASYNC_TCP_SHUT_DOWN, "UID=%64u HOW=0x%x");
FSTRACE_DECL(ASYNC_TCP_SHUT_DOWN_READ, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_SHUT_DOWN_WRITE, "UID=%64u");

void tcp_shut_down(tcp_conn_t *conn, int how, int *perror)
{
    if (inactive(conn)) {
        FSTRACE(ASYNC_TCP_SHUT_DOWN_INACTIVE, conn->uid, how);
        return;
    }
    FSTRACE(ASYNC_TCP_SHUT_DOWN, conn->uid, how);
    *perror = 0;
    if ((how == SHUT_RD || how == SHUT_RDWR) &&
        conn->input.state != SHUT_DOWN) {
        FSTRACE(ASYNC_TCP_SHUT_DOWN_READ, conn->uid);
        (void) shutdown(conn->fd, SHUT_RD);
        set_input_state(conn, SHUT_DOWN);
        conn->input.error = ENOTCONN;
    }
    if ((how == SHUT_WR || how == SHUT_RDWR) &&
        conn->output.state != SHUT_DOWN) {
        FSTRACE(ASYNC_TCP_SHUT_DOWN_WRITE, conn->uid);
        (void) shutdown(conn->fd, SHUT_WR);
        switch (conn->output.state) {
            case CONNECTING:
                break;
            case CONNECTED:
                if (conn->outcursor < conn->outcount)
                    *perror = EPIPE;
                break;
            case ENDED:
                if (conn->outcursor < conn->outcount)
                    *perror = EPIPE;
                else
                    *perror = conn->output.error;
                break;
            default:
                abort();
        }
        set_output_state(conn, SHUT_DOWN);
        reset_output_stream(conn);
    }
}

static bool ancillary_data_info_describes_file_descriptor(int level, int type,
                                                          size_t size)
{
    return level == SOL_SOCKET && type == SCM_RIGHTS && size >= sizeof(int);
}

static void close_fd(void *fd)
{
    close((intptr_t) fd);
}

FSTRACE_DECL(ASYNC_TCP_CLOSE, "UID=%64u");

void tcp_close(tcp_conn_t *conn)
{
    FSTRACE(ASYNC_TCP_CLOSE, conn->uid);
    assert(!conn->connection_closed);
    while (!list_empty(conn->input.ancillary_list)) {
        ancillary_data_t *data =
            (ancillary_data_t *) list_pop_first(conn->input.ancillary_list);
        if (data->kind == ANCILLARY_FD)
            close(data->fd);
        destroy_ancillary_data(data);
    }
    destroy_list(conn->input.ancillary_list);
    while (!list_empty(conn->output.ancillary_list)) {
        ancillary_data_t *data =
            (ancillary_data_t *) list_pop_first(conn->output.ancillary_list);
        if (data->kind == ANCILLARY_ACTION)
            async_execute(conn->async, data->action);
        destroy_ancillary_data(data);
    }
    destroy_list(conn->output.ancillary_list);
    int dummy;
    tcp_shut_down(conn, SHUT_RDWR, &dummy);
    async_unregister(conn->async, conn->fd);
    close(conn->fd);
    conn->connection_closed = true;
    if (conn->input_stream_closed)
        async_wound(conn->async, conn);
}

FSTRACE_DECL(ASYNC_TCP_CLOSE_INPUT_STREAM, "UID=%64u");

void tcp_close_input_stream(tcp_conn_t *conn)
{
    FSTRACE(ASYNC_TCP_CLOSE_INPUT_STREAM, conn->uid);
    assert(!conn->input_stream_closed);
    int dummy;
    tcp_shut_down(conn, SHUT_RD, &dummy);
    conn->input_stream_closed = true;
    if (conn->connection_closed)
        async_wound(conn->async, conn);
}

static void _close(void *obj)
{
    tcp_close_input_stream(obj);
}

FSTRACE_DECL(ASYNC_TCP_REGISTER, "UID=%64u OBJ=%p ACT=%p");

void tcp_register_callback(tcp_conn_t *conn, action_1 action)
{
    FSTRACE(ASYNC_TCP_REGISTER, conn->uid, action.obj, action.act);
    conn->flags |= TCP_FLAG_INGRESS_PENDING;
    conn->notify_input = action;
}

static void _register_callback(void *obj, action_1 action)
{
    tcp_register_callback(obj, action);
}

FSTRACE_DECL(ASYNC_TCP_UNREGISTER, "UID=%64u");

void tcp_unregister_callback(tcp_conn_t *conn)
{
    FSTRACE(ASYNC_TCP_UNREGISTER, conn->uid);
    conn->notify_input = NULL_ACTION_1;
}

static void _unregister_callback(void *obj)
{
    tcp_unregister_callback(obj);
}

static const struct bytestream_1_vt tcp_vt = {
    .read = _read,
    .close = _close,
    .register_callback = _register_callback,
    .unregister_callback = _unregister_callback,
};

bytestream_1 tcp_get_input_stream(tcp_conn_t *conn)
{
    return (bytestream_1) { conn, &tcp_vt };
}

static void no_flush_socket(tcp_conn_t *conn) {}

static int turn_on_sockopt(int fd, int level, int option)
{
    int on = 1;
    return setsockopt(fd, level, option, &on, sizeof on);
}

#ifdef TCP_CORK
FSTRACE_DECL(ASYNC_TCP_FLUSH, "UID=%64u");

static void tcp_flush_socket(tcp_conn_t *conn)
{
    FSTRACE(ASYNC_TCP_FLUSH, conn->uid);
    int status = turn_on_sockopt(conn->fd, IPPROTO_TCP, TCP_NODELAY);
    assert(status >= 0);
}
#endif

static void schedule_user_probe(tcp_conn_t *conn);

FSTRACE_DECL(ASYNC_TCP_REPLENISH_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_REPLENISH_EOF, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_REPLENISH, "UID=%64u GOT=%z");
FSTRACE_DECL(ASYNC_TCP_REPLENISH_DUMP, "UID=%64u DATA=%A");

static void replenish_outbuf(tcp_conn_t *conn)
{
    ssize_t count =
        bytestream_1_read(conn->output_stream, conn->outbuf, OUTBUF_SIZE);
    if (count < 0) {
        FSTRACE(ASYNC_TCP_REPLENISH_FAIL, conn->uid);
        if (errno == EAGAIN) {
            conn->flags |= TCP_FLAG_EGRESS_PENDING;
            conn->flush_socket(conn);
            return;
        }
        set_output_state(conn, ENDED);
        conn->output.error = errno;
        reset_output_stream(conn);
        return;
    }
    if (count == 0) {
        FSTRACE(ASYNC_TCP_REPLENISH_EOF, conn->uid);
        conn->flush_socket(conn);
        shutdown(conn->fd, SHUT_WR);
        set_output_state(conn, SHUT_DOWN);
        conn->output.error = 0;
        reset_output_stream(conn);
        return;
    }
    FSTRACE(ASYNC_TCP_REPLENISH, conn->uid, count);
    FSTRACE(ASYNC_TCP_REPLENISH_DUMP, conn->uid, conn->outbuf, count);
    conn->outcursor = 0;
    conn->outcount = count;
}

#ifndef CMSG_ALIGN
#define CMSG_ALIGN(len) (CMSG_SPACE(len) - CMSG_SPACE(0))
#endif

FSTRACE_DECL(ASYNC_TCP_SEND_SINGLE_DATA_BYTE,
             "UID=%64u ANC-COUNT=%u FD-COUNT=%u ANC-SIZE=%z");

static size_t preview_ancillary_data(tcp_conn_t *conn, bool *single_byte,
                                     unsigned *anc_count)
{
    size_t ancillary_size = 0;
    bool raw = false;           /* ANCILLARY_RAW detected */
    unsigned fd_count = 0;      /* >0 when ANCILLARY_FD detected */
    list_elem_t *ep;
    for (ep = list_get_first(conn->output.ancillary_list), *anc_count = 0; ep;
         ep = list_next(ep), ++*anc_count) {
        ancillary_data_t *data = (ancillary_data_t *) list_elem_get_value(ep);
        switch (data->kind) {
            case ANCILLARY_RAW:
                if (raw || fd_count > 0) /* only one ANCILLARY_RAW at a time */
                    break;
                raw = true;
                ancillary_size = CMSG_ALIGN(data->raw->cmsg_len);
                continue;
            case ANCILLARY_FD:
                if (raw || fd_count >= CMSG_ASYNC_MAX_FD)
                    break;
                fd_count++;
                ancillary_size = CMSG_ALIGN(CMSG_LEN(fd_count * sizeof(int)));
                continue;
            case ANCILLARY_ACTION:
                continue;
            default:
                abort();
        }
        /* We can't send all outstanding ancillary data at once; only
         * consume a single data byte so we can continue sending the
         * remaining ancillary data. */
        FSTRACE(ASYNC_TCP_SEND_SINGLE_DATA_BYTE, conn->uid, *anc_count,
                fd_count, ancillary_size);
        *single_byte = true;
        return ancillary_size;
    }
    *single_byte = false;
    return ancillary_size;
}

FSTRACE_DECL(ASYNC_TCP_SENDMSG_ANCILLARY, "UID=%64u SIZE=%z");
FSTRACE_DECL(ASYNC_TCP_SENDMSG_ANCILLARY_DUMP, "UID=%64u DATA=%A");

static void report_ancillary(tcp_conn_t *conn, uint8_t *ap, size_t anclen)
{
    FSTRACE(ASYNC_TCP_SENDMSG_ANCILLARY, conn->uid, anclen);
    FSTRACE(ASYNC_TCP_SENDMSG_ANCILLARY_DUMP, conn->uid, ap, anclen);
}

static uint8_t *flatten_ancillary_data(tcp_conn_t *conn, size_t ancillary_size,
                                       unsigned anc_count)
{
    uint8_t *ancillary = fsalloc(ancillary_size);
    uint8_t *ap = ancillary;
    struct cmsghdr *fd_cp = NULL;
    unsigned fd_count;          /* valid if fd_cp != NULL */
    list_elem_t *ep;
    for (ep = list_get_first(conn->output.ancillary_list); ep && anc_count--;
         ep = list_next(ep)) {
        ancillary_data_t *data = (ancillary_data_t *) list_elem_get_value(ep);
        switch (data->kind) {
            case ANCILLARY_RAW:
                if (fd_cp) {
                    /* preview_ancillary_data doesn't allow this to
                     * happen, but the logic is forward-looking */
                    size_t anclen = CMSG_LEN(fd_count * sizeof(int));
                    fd_cp->cmsg_len = anclen;
                    report_ancillary(conn, ap, anclen);
                    ap += CMSG_ALIGN(anclen);
                }
                fd_cp = NULL;
                struct cmsghdr *cp = data->raw;
                size_t anclen = cp->cmsg_len;
                memcpy(ap, cp, anclen);
                report_ancillary(conn, ap, anclen);
                ap += CMSG_ALIGN(anclen);
                break;
            case ANCILLARY_FD:
                if (!fd_cp) {
                    fd_cp = (struct cmsghdr *) ap;
                    fd_cp->cmsg_level = SOL_SOCKET;
                    fd_cp->cmsg_type = SCM_RIGHTS;
                    fd_count = 0;
                }
                memcpy(CMSG_DATA(fd_cp) + sizeof data->fd * fd_count++,
                       &data->fd, sizeof data->fd);
                break;
            case ANCILLARY_ACTION:
                break;
            default:
                abort();
        }
        if (fd_cp) {
            size_t anclen = CMSG_LEN(fd_count * sizeof(int));
            fd_cp->cmsg_len = anclen;
            report_ancillary(conn, ap, anclen);
        }
    }
    return ancillary;
}

static void pop_ancillary_data(tcp_conn_t *conn, unsigned anc_count)
{
    while (!list_empty(conn->output.ancillary_list) && anc_count--) {
        ancillary_data_t *data = 
            (ancillary_data_t *) list_pop_first(conn->output.ancillary_list);
        if (data->kind == ANCILLARY_ACTION)
            async_execute(conn->async, data->action);
        destroy_ancillary_data(data);
    }
}

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

FSTRACE_DECL(ASYNC_TCP_SEND_FAIL, "UID=%64u WANT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_SEND, "UID=%64u WANT=%z GOT=%z");
FSTRACE_DECL(ASYNC_TCP_SEND_DUMP, "UID=%64u DATA=%A");
FSTRACE_DECL(ASYNC_TCP_SENDMSG_FAIL, "UID=%64u WANT=%z ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_SENDMSG, "UID=%64u WANT=%z GOT=%z");
FSTRACE_DECL(ASYNC_TCP_SENDMSG_DUMP, "UID=%64u DATA=%A");

// The ancillary semantics of tcp_connection don't align precisely
// with those of sendmsg(2). In particular, the order of delivering
// data and ancillary data is more unpredictable as data is pulled
// rather than pushed.
//
// In the current heuristics, whenever there is more than one piece of
// ancillary data to be sent, only one ancillary data packet is sent
// and a single byte of data (the mandatory minimum) is shipped with
// it. There could be use cases where that leads to data starvation,
// but those use cases should be exceedingly rare. (A workaround: send
// at least one byte of data per ancillary data item.)
static ssize_t transmit(tcp_conn_t *conn, size_t remaining)
{
    assert(remaining > 0);
    uint8_t *point = conn->outbuf + conn->outcursor;
    if (list_empty(conn->output.ancillary_list)) {
        ssize_t count = send(conn->fd, point, remaining, MSG_NOSIGNAL);
        if (count < 0)
            FSTRACE(ASYNC_TCP_SEND_FAIL, conn->uid, remaining);
        else {
            FSTRACE(ASYNC_TCP_SEND, conn->uid, remaining, count);
            FSTRACE(ASYNC_TCP_SEND_DUMP, conn->uid, point, count);
        }
        return count;
    }
    bool single_byte;
    unsigned anc_count;
    size_t ancillary_size =
        preview_ancillary_data(conn, &single_byte, &anc_count);
    struct iovec iov = {
        .iov_base = point,
        .iov_len = single_byte ? 1 : remaining,
    };
    struct msghdr message = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = flatten_ancillary_data(conn, ancillary_size, anc_count),
        .msg_controllen = ancillary_size,
    };
    ssize_t count = sendmsg(conn->fd, &message, MSG_NOSIGNAL);
    if (count < 0)
        FSTRACE(ASYNC_TCP_SENDMSG_FAIL, conn->uid, remaining);
    else {
        FSTRACE(ASYNC_TCP_SENDMSG, conn->uid, remaining, count);
        FSTRACE(ASYNC_TCP_SENDMSG_DUMP, conn->uid, point, count);
        pop_ancillary_data(conn, anc_count);
    }
    fsfree(message.msg_control);
    return count;
}

FSTRACE_DECL(ASYNC_TCP_NO_PUSH_CONNECTED, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_PUSH_CONNECTED, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_NO_PUSH_ENDED, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_PUSH_ENDED, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_NO_PUSH, "UID=%64u");

static void push_output(tcp_conn_t *conn)
{
    ssize_t count, remaining;
    switch (conn->output.state) {
        case CONNECTED:
            remaining = conn->outcount - conn->outcursor;
            if (remaining <= 0) {
                replenish_outbuf(conn);
                remaining = conn->outcount - conn->outcursor;
                if (remaining <= 0) {
                    FSTRACE(ASYNC_TCP_NO_PUSH_CONNECTED, conn->uid);
                    return;
                }
            }
            FSTRACE(ASYNC_TCP_PUSH_CONNECTED, conn->uid);
            count = transmit(conn, remaining);
            if (count < 0) {
                if (errno == EAGAIN) {
                    conn->flags |= TCP_FLAG_EPOLL_SEND;
                    return;
                }
                set_output_state(conn, ENDED);
                conn->output.error = errno;
                conn->outcursor = conn->outcount;
                reset_output_stream(conn);
                return;
            }
            conn->output.byte_count += count;
            conn->outcursor += count;
            schedule_user_probe(conn);
            break;
        case ENDED:
            remaining = conn->outcount - conn->outcursor;
            if (remaining <= 0) {
                FSTRACE(ASYNC_TCP_NO_PUSH_ENDED, conn->uid);
                return;
            }
            FSTRACE(ASYNC_TCP_PUSH_ENDED, conn->uid);
            count = transmit(conn, remaining);
            if (count < 0) {
                if (errno == EAGAIN) {
                    conn->flags |= TCP_FLAG_EPOLL_SEND;
                    return;
                }
                conn->output.error = errno;
                conn->outcursor = conn->outcount;
                return;
            }
            conn->output.byte_count += count;
            conn->outcursor += count;
            if (conn->outcursor >= conn->outcount)
                reset_output_stream(conn);
            else
                schedule_user_probe(conn);
            break;
        default:
            FSTRACE(ASYNC_TCP_NO_PUSH, conn->uid);
    }
}

FSTRACE_DECL(ASYNC_TCP_USER_PROBE_INACTIVE, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_USER_PROBE_CONNECTING, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_USER_PROBE_PUSH, "UID=%64u");

static void user_probe(tcp_conn_t *conn)
{
    if (inactive(conn)) {
        FSTRACE(ASYNC_TCP_USER_PROBE_INACTIVE, conn->uid);
        return;
    }
    conn->flags &= ~TCP_FLAG_EGRESS_PENDING;
    if (conn->output.state == CONNECTING) {
        FSTRACE(ASYNC_TCP_USER_PROBE_CONNECTING, conn->uid);
        return;
    }
    FSTRACE(ASYNC_TCP_USER_PROBE_PUSH, conn->uid);
    push_output(conn);
}

FSTRACE_DECL(ASYNC_TCP_SOCKET_PROBE_INACTIVE, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_SOCKET_PROBE_CONNECTING, "UID=%64u ERROR=%E");
FSTRACE_DECL(ASYNC_TCP_SOCKET_PROBE_IN_PROGRESS, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_SOCKET_PROBE_PUSH, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_SOCKET_PROBE_NOTIFY, "UID=%64u");

static void socket_probe(tcp_conn_t *conn)
{
    if (inactive(conn)) {
        FSTRACE(ASYNC_TCP_SOCKET_PROBE_INACTIVE, conn->uid);
        return;
    }
    conn->flags &= ~(TCP_FLAG_EPOLL_RECV | TCP_FLAG_EPOLL_SEND);
    if (conn->input.state == CONNECTING || conn->output.state == CONNECTING) {
        int status, error;
        socklen_t errlen = sizeof error;
        status = getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &error, &errlen);
        assert(status >= 0);
        FSTRACE(ASYNC_TCP_SOCKET_PROBE_CONNECTING, conn->uid, error);
        switch (error) {
            case EINPROGRESS:
                return;
            default:
                if (conn->input.state == CONNECTING) {
                    set_input_state(conn, ENDED);
                    conn->input.error = error;
                    conn->flags |= TCP_FLAG_INGRESS_PENDING;
                    action_1_perf(conn->notify_input);
                }
                if (conn->output.state == CONNECTING) {
                    set_output_state(conn, ENDED);
                    conn->output.error = error;
                    reset_output_stream(conn);
                }
                return;
            case 0:
                if (conn->input.state == CONNECTING)
                    set_input_state(conn, CONNECTED);
                if (conn->output.state == CONNECTING)
                    set_output_state(conn, CONNECTED);
        }
    }
    FSTRACE(ASYNC_TCP_SOCKET_PROBE_PUSH, conn->uid);
    push_output(conn);
    if (conn->input.state == CONNECTED) {
        conn->flags |= TCP_FLAG_INGRESS_PENDING;
        FSTRACE(ASYNC_TCP_SOCKET_PROBE_NOTIFY, conn->uid);
        action_1_perf(conn->notify_input);
    }
}

FSTRACE_DECL(ASYNC_TCP_SCHEDULE_USER_PROBE, "UID=%64u");

static void schedule_user_probe(tcp_conn_t *conn)
{
    FSTRACE(ASYNC_TCP_SCHEDULE_USER_PROBE, conn->uid);
    async_execute(conn->async, (action_1) { conn, (act_1) user_probe });
}

FSTRACE_DECL(ASYNC_TCP_SCHEDULE_SOCKET_PROBE, "UID=%64u");

static void schedule_socket_probe(tcp_conn_t *conn)
{
    FSTRACE(ASYNC_TCP_SCHEDULE_SOCKET_PROBE, conn->uid);
    async_execute(conn->async, (action_1) { conn, (act_1) socket_probe });
}

void set_output_stream(tcp_conn_t *conn, bytestream_1 output_stream)
{
    if (inactive(conn))
        return;
    bytestream_1_close(conn->output_stream);
    conn->output_stream = output_stream;
    bytestream_1_register_callback(output_stream,
                                   (action_1) { conn, (act_1) user_probe });
    schedule_user_probe(conn);
}

FSTRACE_DECL(ASYNC_TCP_SET_OUTPUT_STREAM, "UID=%64u OBJ=%p");

void tcp_set_output_stream(tcp_conn_t *conn, bytestream_1 output_stream)
{
    FSTRACE(ASYNC_TCP_SET_OUTPUT_STREAM, conn->uid, output_stream.obj);
    set_output_stream(conn, output_stream);
}

static tcp_conn_t *adopt_connection(async_t *async, uint64_t uid, int connfd);

FSTRACE_DECL(ASYNC_TCP_CONNECT, "UID=%64u ASYNC=%p FROM=%a TO=%a");
FSTRACE_DECL(ASYNC_TCP_CONNECT_SOCKET_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_CONNECT_BIND_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_CONNECT_GETFL_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_CONNECT_SETFL_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_CONNECT_IMMEDIATE, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_CONNECT_IN_PROGRESS, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_CONNECT_FAIL, "UID=%64u ERRNO=%e");

/* Somewhat shortsighted to assume that 'from' and 'to' have to have the
 * same 'addrlen' (see unix(7)). */
tcp_conn_t *tcp_connect(async_t *async, const struct sockaddr *from,
                        const struct sockaddr *to, socklen_t addrlen)
{
    uint64_t uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_TCP_CONNECT, uid, async, from, addrlen, to, addrlen);
    int connfd = socket(to->sa_family, SOCK_STREAM, 0);
    if (connfd < 0) {
        FSTRACE(ASYNC_TCP_CONNECT_SOCKET_FAIL, uid);
        return NULL;
    }
    int status;
    if (from != NULL) {
        status = bind(connfd, from, addrlen);
        if (status < 0) {
            FSTRACE(ASYNC_TCP_CONNECT_BIND_FAIL, uid);
            int err = errno;
            close(connfd);
            errno = err;
            return NULL;
        }
    }
    status = fcntl(connfd, F_GETFL, 0);
    if (status < 0) {
        FSTRACE(ASYNC_TCP_CONNECT_GETFL_FAIL, uid);
        int err = errno;
        close(connfd);
        errno = err;
        return NULL;
    }
    if (fcntl(connfd, F_SETFL, status | O_NONBLOCK) < 0) {
        FSTRACE(ASYNC_TCP_CONNECT_GETFL_FAIL, uid);
        int err = errno;
        close(connfd);
        errno = err;
        return NULL;
    }
    status = connect(connfd, to, addrlen);
    if (status >= 0) {
        FSTRACE(ASYNC_TCP_CONNECT_IMMEDIATE, uid);
        tcp_conn_t *conn = adopt_connection(async, uid, connfd);
        /* Invoke socket_probe changes the status to connected. */
        if (conn)
            schedule_socket_probe(conn);
        return conn;
    }
    if (errno == EINPROGRESS) {
        FSTRACE(ASYNC_TCP_CONNECT_IN_PROGRESS, uid);
        /* We must not invoke socket_probe before epoll tells us to. */
        return adopt_connection(async, uid, connfd);
    }
    FSTRACE(ASYNC_TCP_CONNECT_FAIL, uid);
    int err = errno;
    close(connfd);
    errno = err;
    return NULL;
}

FSTRACE_DECL(ASYNC_TCP_SERVER_PROBE_INACTIVE, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_SERVER_PROBE, "UID=%64u");

static void server_probe(tcp_server_t *server)
{
    if (server->async == NULL) {
        FSTRACE(ASYNC_TCP_SERVER_PROBE_INACTIVE, server->uid);
        return;
    }
    FSTRACE(ASYNC_TCP_SERVER_PROBE, server->uid);
    action_1_perf(server->notify);
}

static tcp_server_t *adopt_server(async_t *async, uint64_t uid, int serverfd);

FSTRACE_DECL(ASYNC_TCP_LISTEN, "UID=%64u ASYNC=%p ADDRESS=%a");
FSTRACE_DECL(ASYNC_TCP_LISTEN_SOCKET_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_LISTEN_REUSE_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_LISTEN_BIND_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_LISTEN_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_LISTEN_ADOPT, "UID=%64u FD=%d");

tcp_server_t *tcp_listen(async_t *async, const struct sockaddr *address,
                         socklen_t addrlen)
{
    uint64_t uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_TCP_LISTEN, uid, async, address, addrlen);
    int sockfd = socket(address->sa_family, SOCK_STREAM, 0);
    if (sockfd < 0) {
        FSTRACE(ASYNC_TCP_LISTEN_SOCKET_FAIL, uid);
        return NULL;
    }
    int status = turn_on_sockopt(sockfd, SOL_SOCKET, SO_REUSEADDR);
    if (status < 0) {
        FSTRACE(ASYNC_TCP_LISTEN_REUSE_FAIL, uid);
        int err = errno;
        close(sockfd);
        errno = err;
        return NULL;
    }
    status = bind(sockfd, address, addrlen);
    if (status < 0) {
        FSTRACE(ASYNC_TCP_LISTEN_BIND_FAIL, uid);
        int err = errno;
        close(sockfd);
        errno = err;
        return NULL;
    }
    status = listen(sockfd, 128);
    if (status < 0) {
        FSTRACE(ASYNC_TCP_LISTEN_FAIL, uid);
        int err = errno;
        close(sockfd);
        errno = err;
        return NULL;
    }
    FSTRACE(ASYNC_TCP_LISTEN_ADOPT, uid, sockfd);
    return adopt_server(async, uid, sockfd);
}

FSTRACE_DECL(ASYNC_TCP_CLOSE_SERVER, "UID=%64u");

void tcp_close_server(tcp_server_t *server)
{
    assert(server->async != NULL);
    FSTRACE(ASYNC_TCP_CLOSE_SERVER, server->uid);
    async_unregister(server->async, server->fd);
    close(server->fd);
    async_wound(server->async, server);
    server->async = NULL;
}

FSTRACE_DECL(ASYNC_TCP_REGISTER_SERVER, "UID=%64u OBJ=%p ACT=%p");

void tcp_register_server_callback(tcp_server_t *server, action_1 action)
{
    FSTRACE(ASYNC_TCP_REGISTER_SERVER, server->uid, action.obj, action.act);
    server->notify = action;
}

FSTRACE_DECL(ASYNC_TCP_UNREGISTER_SERVER, "UID=%64u");

void tcp_unregister_server_callback(tcp_server_t *server)
{
    FSTRACE(ASYNC_TCP_UNREGISTER_SERVER, server->uid);
    server->notify = NULL_ACTION_1;
}

FSTRACE_DECL(ASYNC_TCP_ACCEPT_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_ACCEPT, "UID=%64u UID=%64u FROM=%a FD=%d");

tcp_conn_t *tcp_accept(tcp_server_t *server, struct sockaddr *addr,
                       socklen_t *addrlen)
{
    assert(server->async != NULL);
    int connfd = accept(server->fd, addr, addrlen);
    if (connfd < 0) {
        FSTRACE(ASYNC_TCP_ACCEPT_FAIL, server->uid);
        return NULL;
    }
    uint64_t uid = fstrace_get_unique_id();
    const socklen_t length = addrlen != NULL ? *addrlen : 0;
    FSTRACE(ASYNC_TCP_ACCEPT, server->uid, uid, addr, length, connfd);
    tcp_conn_t *conn = adopt_connection(server->async, uid, connfd);
    if (conn)
        schedule_socket_probe(conn);
    return conn;
}

int tcp_get_fd(tcp_conn_t *conn)
{
    return conn->fd;
}

FSTRACE_DECL(ASYNC_TCP_PEEK_ANCILLARY, "UID=%64u LEVEL=%d TYPE=%d SIZE=%z");

ssize_t tcp_peek_ancillary_data(tcp_conn_t *conn, int *level, int *type)
{
    list_elem_t *ep = list_get_first(conn->input.ancillary_list);
    if (!ep) {
        errno = EAGAIN;
        return -1;
    }
    ancillary_data_t *data = (ancillary_data_t *) list_elem_get_value(ep);
    if (data->kind != ANCILLARY_RAW) {
        /* Calling tcp_recv_fd() reorganizes ancillary_list as a side effect. */
        errno = EINVAL;
        return -1;
    }
    size_t size = ancillary_data_info(data->raw, level, type);
    FSTRACE(ASYNC_TCP_PEEK_ANCILLARY, conn->uid, *level, *type, size);
    return size;
}

FSTRACE_DECL(ASYNC_TCP_RECV_ANCILLARY_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_RECV_ANCILLARY, "UID=%64u SIZE=%z");
FSTRACE_DECL(ASYNC_TCP_RECV_ANCILLARY_DUMP, "UID=%64u DATA=%A");

ssize_t tcp_recv_ancillary_data(tcp_conn_t *conn, void *buf, size_t size)
{
    list_elem_t *ep = list_get_first(conn->input.ancillary_list);
    if (!ep) {
        errno = EAGAIN;
        FSTRACE(ASYNC_TCP_RECV_ANCILLARY_FAIL, conn->uid);
        return -1;
    }
    ancillary_data_t *data = (ancillary_data_t *) list_elem_get_value(ep);
    if (data->kind != ANCILLARY_RAW) {
        /* Calling tcp_recv_fd() reorganizes ancillary_list as a side effect. */
        errno = EINVAL;
        FSTRACE(ASYNC_TCP_RECV_ANCILLARY_FAIL, conn->uid);
        return -1;
    }
    list_pop_first(conn->input.ancillary_list);
    size_t available = ancillary_data_size(data->raw);
    if (available > size)
        available = size;
    memcpy(buf, CMSG_DATA(data->raw), available);
    FSTRACE(ASYNC_TCP_RECV_ANCILLARY, conn->uid, available);
    FSTRACE(ASYNC_TCP_RECV_ANCILLARY_DUMP, conn->uid, buf, available);
    destroy_ancillary_data(data);
    return available;
}

FSTRACE_DECL(ASYNC_TCP_RECV_FD_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_TCP_RECV_FD, "UID=%64u FD=%d");

int tcp_recv_fd(tcp_conn_t *conn)
{
    list_elem_t *ep = list_get_first(conn->input.ancillary_list);
    if (!ep) {
        errno = EAGAIN;
        FSTRACE(ASYNC_TCP_RECV_FD_FAIL, conn->uid);
        return -1;
    }
    ancillary_data_t *data = (ancillary_data_t *) list_elem_get_value(ep);
    if (data->kind == ANCILLARY_FD) {
        int fd = data->fd;
        list_pop_first(conn->input.ancillary_list);
        destroy_ancillary_data(data);
        FSTRACE(ASYNC_TCP_RECV_FD, conn->uid, fd);
        return fd;
    }
    assert(data->kind == ANCILLARY_RAW);
    int level, type;
    size_t size = ancillary_data_info(data->raw, &level, &type);
    if (!ancillary_data_info_describes_file_descriptor(level, type, size)) {
        errno = EPROTO;
        FSTRACE(ASYNC_TCP_RECV_FD_FAIL, conn->uid);
        return -1;
    }
    list_pop_first(conn->input.ancillary_list);
    int fd;
    for (unsigned i = size / sizeof fd - 1; i > 0; i--) {
        memcpy(&fd, CMSG_DATA(data->raw) + sizeof fd * i, sizeof fd);
        list_prepend(conn->input.ancillary_list, make_fd_ancillary(fd));
    }
    memcpy(&fd, CMSG_DATA(data->raw), sizeof fd);
    destroy_ancillary_data(data);
    FSTRACE(ASYNC_TCP_RECV_FD, conn->uid, fd);
    return fd;
}

FSTRACE_DECL(ASYNC_TCP_PEEK_RECEIVED_FDS, "UID=%64u");
FSTRACE_DECL(ASYNC_TCP_PEEK_RECEIVED_FDS_FD, "UID=%64u FD=%d");

list_t *tcp_peek_received_fds(tcp_conn_t *conn)
{
    FSTRACE(ASYNC_TCP_PEEK_RECEIVED_FDS, conn->uid);
    list_t *fds = make_list();
    for (list_elem_t *ep = list_get_first(conn->input.ancillary_list);
         ep != NULL; ep = list_next(ep)) {
        ancillary_data_t *data = (ancillary_data_t *) list_elem_get_value(ep);
        if (data->kind == ANCILLARY_FD) {
            list_append(fds, as_integer(data->fd));
            continue;
        }
        assert(data->kind == ANCILLARY_RAW);
        int level, type;
        size_t size = ancillary_data_info(data->raw, &level, &type);
        if (!ancillary_data_info_describes_file_descriptor(level, type, size))
            continue;
        int fd;
        unsigned fd_count = size / sizeof fd;
        for (unsigned i = 0; i < fd_count; i++) {
            memcpy(&fd, CMSG_DATA(data->raw) + i * sizeof fd, sizeof fd);
            FSTRACE(ASYNC_TCP_PEEK_RECEIVED_FDS_FD, conn->uid, fd);
            list_append(fds, as_integer(fd));
        }
    }
    return fds;
}

FSTRACE_DECL(ASYNC_TCP_SEND_ANCILLARY, "UID=%64u SIZE=%z");
FSTRACE_DECL(ASYNC_TCP_SEND_ANCILLARY_DUMP, "UID=%64u DATA=%A");

ssize_t tcp_send_ancillary_data(tcp_conn_t *conn, int level, int type,
                                const void *buf, size_t size)
{
    FSTRACE(ASYNC_TCP_SEND_ANCILLARY, conn->uid, size);
    FSTRACE(ASYNC_TCP_SEND_ANCILLARY_DUMP, conn->uid, buf, size);
    struct cmsghdr *cp = fsalloc(CMSG_SPACE(size));
    cp->cmsg_level = level;
    cp->cmsg_type = type;
    cp->cmsg_len = CMSG_LEN(size);
    memcpy(CMSG_DATA(cp), buf, size);
    list_append(conn->output.ancillary_list, make_raw_ancillary(cp));
    return size;
}

FSTRACE_DECL(ASYNC_TCP_SEND_FD, "UID=%64u FD=%d");

int tcp_send_fd(tcp_conn_t *conn, int fd, bool close_after_sending)
{
    list_append(conn->output.ancillary_list, make_fd_ancillary(fd));
    if (close_after_sending) {
        action_1 closer = { (void *) (intptr_t) fd, close_fd };
        tcp_mark_ancillary_data(conn, closer);
    }
    FSTRACE(ASYNC_TCP_SEND_FD, conn->uid, fd);
    return 0;
}

FSTRACE_DECL(ASYNC_TCP_MARK_ANCILLARY, "UID=%64u OBJ=%p ACT=%p");

void tcp_mark_ancillary_data(tcp_conn_t *conn, action_1 action)
{
    FSTRACE(ASYNC_TCP_MARK_ANCILLARY, conn->uid, action.obj, action.act);
    list_append(conn->output.ancillary_list, make_action_ancillary(action));
}

FSTRACE_DECL(ASYNC_TCP_ADOPT_CREATE, "UID=%64u PTR=%p");
FSTRACE_DECL(ASYNC_TCP_ADOPT_FAIL, "UID=%64u FD=%d ERRNO=%e");

static tcp_conn_t *adopt_connection(async_t *async, uint64_t uid, int connfd)
{
    tcp_conn_t *conn = fsalloc(sizeof *conn);
    FSTRACE(ASYNC_TCP_ADOPT_CREATE, uid, conn);
    conn->async = async;
    conn->uid = uid;
    conn->output_stream = drystream;
    conn->outcursor = conn->outcount = 0;
    conn->connection_closed = conn->input_stream_closed = false;
    conn->input.error = conn->output.error = 0;
    conn->input.ancillary_list = make_list();
    conn->output.ancillary_list = make_list();
    tcp_unregister_callback(conn);
    conn->fd = connfd;
#ifdef SO_NOSIGPIPE
    /* This is how OSX disables SIGPIPE. Linux uses MSG_NOSIGNAL in send. */
    if (turn_on_sockopt(conn->fd, SOL_SOCKET, SO_NOSIGPIPE) < 0)
        goto fail;
#endif
        /* address the ack delay problem by corking where available */
#ifdef TCP_CORK
    if (turn_on_sockopt(conn->fd, IPPROTO_TCP, TCP_CORK) >= 0)
        conn->flush_socket = tcp_flush_socket;
    else if (errno == EOPNOTSUPP)
        conn->flush_socket = no_flush_socket;
    else
        goto fail;
#else
    conn->flush_socket = no_flush_socket;
#endif
    action_1 socket_probe_cb = { conn, (act_1) socket_probe };
    async_register(async, conn->fd, socket_probe_cb);
    conn->input.state = conn->output.state = CONNECTING;
    conn->flags = TCP_FLAG_EPOLL_SEND | TCP_FLAG_INGRESS_PENDING;
    return conn;

fail:
    FSTRACE(ASYNC_TCP_ADOPT_FAIL, uid, connfd);
    close(conn->fd);
    fsfree(conn);
    return NULL;
}

FSTRACE_DECL(ASYNC_TCP_ADOPT, "UID=%64u FD=%d");

tcp_conn_t *tcp_adopt_connection(async_t *async, int connfd)
{
    uint64_t uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_TCP_ADOPT, uid, connfd);
    return adopt_connection(async, uid, connfd);
}

void tcp_get_statistics_1(tcp_conn_t *conn, tcp_statistics_1 *stats)
{
    stats->bytes_received = conn->input.byte_count;
    stats->bytes_sent = conn->output.byte_count;
    stats->bytes_to_be_sent = conn->outcount - conn->outcursor;
    stats->flags = conn->flags;
    if (conn->input.state == CONNECTED)
        stats->flags |= TCP_FLAG_INGRESS_LIVE;
    if (conn->output.state == CONNECTED)
        stats->flags |= TCP_FLAG_EGRESS_LIVE;
}

int tcp_get_server_fd(tcp_server_t *server)
{
    return server->fd;
}

FSTRACE_DECL(ASYNC_TCP_ADOPT_SERVER_CREATE, "UID=%64u PTR=%p");

static tcp_server_t *adopt_server(async_t *async, uint64_t uid, int serverfd)
{
    tcp_server_t *server = fsalloc(sizeof *server);
    FSTRACE(ASYNC_TCP_ADOPT_SERVER_CREATE, uid, server);
    server->async = async;
    server->uid = uid;
    server->fd = serverfd;
    server->notify = NULL_ACTION_1;
    async_register(async, server->fd,
                   (action_1) { server, (act_1) server_probe });
    return server;
}

FSTRACE_DECL(ASYNC_TCP_ADOPT_SERVER, "UID=%64u FD=%d");

tcp_server_t *tcp_adopt_server(async_t *async, int serverfd)
{
    uint64_t uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_TCP_ADOPT_SERVER, uid, serverfd);
    return adopt_server(async, uid, serverfd);
}
