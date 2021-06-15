#include <limits.h>
#include <unistd.h>
#ifdef __linux__
#define USE_EPOLL 1
#include <sys/epoll.h>
#else /* assume DARWIN */
#define USE_EPOLL 0
#include <sys/event.h>
#include <sys/types.h>
#endif
#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>

#include <fsdyn/avltree.h>
#include <fsdyn/fsalloc.h>
#include <fsdyn/list.h>
#include <fsdyn/priority_queue.h>
#include <fstrace.h>
#include <unixkit/unixkit.h>
#ifdef HAVE_EXECINFO
#include <execinfo.h>
#endif
#include <assert.h>

#include "async.h"
#include "async_imp.h"
#include "async_version.h"

static int timer_cmp(const void *t1, const void *t2)
{
    const async_timer_t *timer1 = t1;
    const async_timer_t *timer2 = t2;
    if (timer1->expires < timer2->expires)
        return -1;
    if (timer1->expires > timer2->expires)
        return 1;
    if (timer1->seqno < timer2->seqno)
        return -1;
    if (timer1->seqno > timer2->seqno)
        return 1;
    return 0;
}

static void timer_reloc(const void *t, void *loc)
{
    ((async_timer_t *) t)->loc = loc;
}

static int intptr_cmp(const void *i1, const void *i2)
{
    if ((intptr_t) i1 < (intptr_t) i2)
        return -1;
    if ((intptr_t) i1 > (intptr_t) i2)
        return 1;
    return 0;
}

static int cloexec(int fd)
{
    int status = fcntl(fd, F_GETFD, 0);
    if (status < 0)
        return status;
    return fcntl(fd, F_SETFD, status | FD_CLOEXEC);
}

static int nonblock(int fd)
{
    int status = fcntl(fd, F_GETFL, 0);
    if (status < 0)
        return status;
    return fcntl(fd, F_SETFL, status | O_NONBLOCK);
}

FSTRACE_DECL(ASYNC_EPOLL_CREATE_FAILED, "ERRNO=%e");
FSTRACE_DECL(ASYNC_CLOEXEC_FAILED, "ERRNO=%e");
FSTRACE_DECL(ASYNC_CREATE, "UID=%64u PTR=%p FD=%d");

async_t *make_async(void)
{
    async_t *async = fsalloc(sizeof *async);
    async->uid = fstrace_get_unique_id();
#if USE_EPOLL
    int fd = epoll_create(1);
#else
    int fd = kqueue();
#endif
    if (fd < 0) {
        FSTRACE(ASYNC_EPOLL_CREATE_FAILED);
        fsfree(async);
        return NULL;
    }
    if (cloexec(fd) < 0) {
        FSTRACE(ASYNC_CLOEXEC_FAILED);
        fsfree(async);
        return NULL;
    }

    FSTRACE(ASYNC_CREATE, async->uid, async, fd);
    async->poll_fd = fd;
    async->immediate = make_list();
    async->timers = make_priority_queue(timer_cmp, timer_reloc);
    async->registrations = make_avl_tree(intptr_cmp);
    async->wakeup_fd = -1;
    async->wounded_objects = make_list();
#ifdef __MACH__
    host_get_clock_service(mach_host_self(), SYSTEM_CLOCK, &async->mach_clock);
#endif
    (void) async_now(async); /* initialize async->recent */
    return async;
}

static async_timer_t *earliest_timer(async_t *async)
{
    async_timer_t *timed = (async_timer_t *) priorq_peek(async->timers);
    if (list_empty(async->immediate))
        return timed;
    async_timer_t *immediate =
        (async_timer_t *) list_elem_get_value(list_get_first(async->immediate));
    if (timed && timer_cmp(timed, immediate) < 0)
        return timed;
    return immediate;
}

static void finish_wounded_object(async_t *async)
{
    fsfree((void *) list_pop_first(async->wounded_objects));
}

static void finish_wounded_objects(async_t *async)
{
    while (!list_empty(async->wounded_objects))
        finish_wounded_object(async);
}

FSTRACE_DECL(ASYNC_DESTROY, "UID=%64u");

void destroy_async(async_t *async)
{
    FSTRACE(ASYNC_DESTROY, async->uid);
    async_timer_t *timer;
    while ((timer = earliest_timer(async)) != NULL)
        async_timer_cancel(async, timer);
    destroy_priority_queue(async->timers);
    destroy_list(async->immediate);
    avl_elem_t *element;
    while ((element = avl_tree_get_first(async->registrations)) != NULL) {
        int fd = (intptr_t) avl_elem_get_key(element);
        async_unregister(async, fd);
    }
    destroy_avl_tree(async->registrations);
#ifdef __MACH__
    mach_port_deallocate(mach_task_self(), async->mach_clock);
#endif
    (void) close(async->poll_fd);
    finish_wounded_objects(async);
    destroy_list(async->wounded_objects);
    fsfree(async);
}

FSTRACE_DECL(ASYNC_NOW, "UID=%64u TIME=%64u");

uint64_t async_now(async_t *async)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    async->recent = (uint64_t) t.tv_sec * 1000000000 + t.tv_nsec;
#elif defined(__MACH__)
    mach_timespec_t t;
    clock_get_time(async->mach_clock, &t);
    async->recent = (uint64_t) t.tv_sec * 1000000000 + t.tv_nsec;
#else
    struct timeval t;
    gettimeofday(&t, NULL);
    async->recent = (uint64_t) t.tv_sec * 1000000000 + t.tv_usec * 1000;
#endif
    FSTRACE(ASYNC_NOW, async->uid, async->recent);
    return async->recent;
}

FSTRACE_DECL(ASYNC_WAKE_UP, "UID=%64u");

static void wake_up(async_t *async)
{
    FSTRACE(ASYNC_WAKE_UP, async->uid);
    if (async->wakeup_fd >= 0 && write(async->wakeup_fd, wake_up, 1) < 0)
        assert(errno == EAGAIN);
}

enum {
    BT_DEPTH = 31,
};

FSTRACE_DECL(ASYNC_TIMER_BT, "UID=%64u BT=%s");

static async_timer_t *new_timer(async_t *async, bool immediate,
                                uint64_t expires, action_1 action)
{
    async_timer_t *timer = fsalloc(sizeof *timer);
    timer->expires = expires;
    timer->seqno = fstrace_get_unique_id();
    timer->immediate = immediate;
    timer->action = action;
    timer->stack_trace = NULL;
#ifdef HAVE_EXECINFO
    if (FSTRACE_ENABLED(ASYNC_TIMER_BT)) {
        timer->stack_trace = fscalloc(BT_DEPTH, sizeof(void *));
        backtrace(timer->stack_trace, BT_DEPTH);
    }
#endif
    return timer;
}

static async_timer_t *timer_start(async_t *async, uint64_t expires,
                                  action_1 action)
{
    async_timer_t *timer = new_timer(async, false, expires, action);
    priorq_enqueue(async->timers, timer);
    wake_up(async);
    return timer;
}

FSTRACE_DECL(ASYNC_TIMER_START,
             "UID=%64u PTR=%p ASYNC=%64u EXPIRES=%64u OBJ=%p ACT=%p");

async_timer_t *async_timer_start(async_t *async, uint64_t expires,
                                 action_1 action)
{
    async_timer_t *timer = timer_start(async, expires, action);
    FSTRACE(ASYNC_TIMER_START, timer->seqno, timer, async->uid, expires,
            action.obj, action.act);
    return timer;
}

static void timer_cancel(async_t *async, async_timer_t *timer)
{
    if (timer->immediate)
        list_remove(async->immediate, timer->loc);
    else
        priorq_remove(async->timers, timer->loc);
    fsfree(timer->stack_trace);
    fsfree(timer);
}

FSTRACE_DECL(ASYNC_TIMER_CANCEL, "UID=%64u");

void async_timer_cancel(async_t *async, async_timer_t *timer)
{
    FSTRACE(ASYNC_TIMER_CANCEL, timer->seqno);
    timer_cancel(async, timer);
}

FSTRACE_DECL(ASYNC_EVENT_CREATE, "UID=%64u PTR=%p ASYNC=%64u OBJ=%p ACT=%p");

async_event_t *make_async_event(async_t *async, action_1 action)
{
    async_event_t *event = fsalloc(sizeof *event);
    event->async = async;
    event->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_EVENT_CREATE, event->uid, event, async->uid, action.obj,
            action.act);
    event->state = ASYNC_EVENT_IDLE;
    event->action = action;
    event->stack_trace = NULL;
    return event;
}

static const char *trace_event_state(void *pstate)
{
    switch (*(async_event_state_t *) pstate) {
        case ASYNC_EVENT_IDLE:
            return "ASYNC_EVENT_IDLE";
        case ASYNC_EVENT_TRIGGERED:
            return "ASYNC_EVENT_TRIGGERED";
        case ASYNC_EVENT_CANCELED:
            return "ASYNC_EVENT_CANCELED";
        case ASYNC_EVENT_ZOMBIE:
            return "ASYNC_EVENT_ZOMBIE";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNC_EVENT_SET_STATE, "UID=%64u OLD=%I NEW=%I");

static void event_set_state(async_event_t *event, async_event_state_t state)
{
    FSTRACE(ASYNC_EVENT_SET_STATE, event->uid, trace_event_state, &event->state,
            trace_event_state, &state);
    event->state = state;
}

FSTRACE_DECL(ASYNC_EVENT_PERF, "UID=%64u");

static void event_perf(async_event_t *event)
{
    FSTRACE(ASYNC_EVENT_PERF, event->uid);
    switch (event->state) {
        case ASYNC_EVENT_TRIGGERED:
            event_set_state(event, ASYNC_EVENT_IDLE);
            action_1_perf(event->action);
            break;
        case ASYNC_EVENT_CANCELED:
            event_set_state(event, ASYNC_EVENT_IDLE);
            break;
        case ASYNC_EVENT_ZOMBIE:
            fsfree(event);
            break;
        default:
            assert(false);
    }
}

FSTRACE_DECL(ASYNC_EVENT_TRIGGER, "UID=%64u");

void async_event_trigger(async_event_t *event)
{
    FSTRACE(ASYNC_EVENT_TRIGGER, event->uid);
    switch (event->state) {
        case ASYNC_EVENT_IDLE:
            event_set_state(event, ASYNC_EVENT_TRIGGERED);
            async_execute(event->async,
                          (action_1) { event, (act_1) event_perf });
            break;
        case ASYNC_EVENT_TRIGGERED:
            break;
        case ASYNC_EVENT_CANCELED:
            event_set_state(event, ASYNC_EVENT_TRIGGERED);
            break;
        default:
            assert(false);
    }
}

FSTRACE_DECL(ASYNC_EVENT_CANCEL, "UID=%64u");

void async_event_cancel(async_event_t *event)
{
    FSTRACE(ASYNC_EVENT_CANCEL, event->uid);
    switch (event->state) {
        case ASYNC_EVENT_IDLE:
        case ASYNC_EVENT_CANCELED:
            break;
        case ASYNC_EVENT_TRIGGERED:
            event_set_state(event, ASYNC_EVENT_CANCELED);
            break;
        default:
            assert(false);
    }
}

FSTRACE_DECL(ASYNC_EVENT_DESTROY, "UID=%64u");

void destroy_async_event(async_event_t *event)
{
    FSTRACE(ASYNC_EVENT_DESTROY, event->uid);
    switch (event->state) {
        case ASYNC_EVENT_IDLE:
            fsfree(event);
            break;
        case ASYNC_EVENT_TRIGGERED:
        case ASYNC_EVENT_CANCELED:
            event_set_state(event, ASYNC_EVENT_ZOMBIE);
            break;
        default:
            assert(false);
    }
}

static async_timer_t *execute(async_t *async, action_1 action)
{
    async_timer_t *timer = new_timer(async, true, async->recent, action);
    timer->loc = list_append(async->immediate, timer);
    wake_up(async);
    return timer;
}

FSTRACE_DECL(ASYNC_EXECUTE,
             "UID=%64u PTR=%p ASYNC=%p EXPIRES=%64u OBJ=%p ACT=%p");

async_timer_t *async_execute(async_t *async, action_1 action)
{
    async_timer_t *timer = execute(async, action);
    FSTRACE(ASYNC_EXECUTE, timer->seqno, timer, async, timer->expires,
            action.obj, action.act);
    return timer;
}

FSTRACE_DECL(ASYNC_WOUND, "UID=%64u PTR=%p ASYNC=%p OBJ=%p");

void async_wound(async_t *async, void *object)
{
    list_append(async->wounded_objects, object);
    action_1 dealloc_cb = { async, (act_1) finish_wounded_object };
    async_timer_t *timer = execute(async, dealloc_cb);
    FSTRACE(ASYNC_WOUND, timer->seqno, timer, async, object);
}

int async_fd(async_t *async)
{
    return async->poll_fd;
}

static int64_t ns_remaining(async_t *async, async_timer_t *timer)
{
    return timer->expires - async_now(async);
}

static char *emit_char(char *p, const char *end, char c)
{
    if (p < end)
        *p++ = c;
    return p;
}

static char *emit_address(char *p, const char *end, void *address)
{
    char addrbuf[20];
    snprintf(addrbuf, sizeof addrbuf, "%p", address);
    const char *q = addrbuf;
    while (*q)
        p = emit_char(p, end, *q++);
    return p;
}

static void emit_timer_backtrace(async_timer_t *timer)
{
    enum { BUF_SIZE = BT_DEPTH * 20 };
    char buf[BUF_SIZE];
    const char *end = buf + BUF_SIZE - 1; /* leave room for '\0' */
    char *p = emit_address(buf, end, timer->stack_trace[0]);
    int i;
    for (i = 1; i < BT_DEPTH && timer->stack_trace[i]; i++) {
        p = emit_char(p, end, '`');
        p = emit_address(p, end, timer->stack_trace[i]);
    }
    *p = '\0';
    FSTRACE(ASYNC_TIMER_BT, timer->seqno, buf);
}

FSTRACE_DECL(ASYNC_POLL_NO_TIMERS, "UID=%64u");
FSTRACE_DECL(ASYNC_POLL_TIMEOUT, "UID=%64u OBJ=%p ACT=%p");
FSTRACE_DECL(ASYNC_POLL_NEXT_TIMER, "UID=%64u EXPIRES=%64u");
FSTRACE_DECL(ASYNC_POLL_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_POLL_SPURIOUS, "UID=%64u");
FSTRACE_DECL(ASYNC_POLL_CALL_BACK, "UID=%64u EVENT=%64u");

int async_poll(async_t *async, uint64_t *pnext_timeout)
{
    async_timer_t *timer = earliest_timer(async);
    if (timer == NULL) {
        *pnext_timeout = (uint64_t) -1;
        FSTRACE(ASYNC_POLL_NO_TIMERS, async->uid);
    } else {
        if (ns_remaining(async, timer) <= 0) {
            action_1 action = timer->action;
            FSTRACE(ASYNC_POLL_TIMEOUT, timer->seqno, timer->action.obj,
                    timer->action.act);
            if (FSTRACE_ENABLED(ASYNC_TIMER_BT) && timer->stack_trace)
                emit_timer_backtrace(timer);
            timer_cancel(async, timer);
            action_1_perf(action);
            *pnext_timeout = 0;
            return 0;
        }
        FSTRACE(ASYNC_POLL_NEXT_TIMER, async->uid, timer->expires);
        *pnext_timeout = timer->expires;
    }
    int count;
#if USE_EPOLL
    struct epoll_event epoll_event;
    count = epoll_wait(async->poll_fd, &epoll_event, 1, 0);
    if (count < 0) {
        FSTRACE(ASYNC_POLL_FAIL, async->uid);
        return count;
    }
    if (count == 0) {
        FSTRACE(ASYNC_POLL_SPURIOUS, async->uid);
        return 0;
    }
    async_event_t *event = epoll_event.data.ptr;
#else
    struct kevent kq_event;
    struct timespec immediate = { 0, 0 };
    count = kevent(async->poll_fd, NULL, 0, &kq_event, 1, &immediate);
    if (count < 0) {
        FSTRACE(ASYNC_POLL_FAIL, async->uid);
        return count;
    }
    if (count == 0) {
        FSTRACE(ASYNC_POLL_SPURIOUS, async->uid);
        return 0;
    }
    async_event_t *event = kq_event.udata;
#endif
    FSTRACE(ASYNC_POLL_CALL_BACK, async->uid, event->uid);
    async_event_trigger(event);
    *pnext_timeout = 0;
    return 0;
}

FSTRACE_DECL(ASYNC_QUIT_LOOP, "UID=%64u");

void async_quit_loop(async_t *async)
{
    FSTRACE(ASYNC_QUIT_LOOP, async->uid);
    async->quit = true;
    wake_up(async);
}

FSTRACE_DECL(ASYNC_FLUSH, "UID=%64u EXPIRES=%64u");
FSTRACE_DECL(ASYNC_FLUSH_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_FLUSH_TIMERS_PENDING, "UID=%64u");
FSTRACE_DECL(ASYNC_FLUSH_EXPIRED, "UID=%64u");

int async_flush(async_t *async, uint64_t expires)
{
    FSTRACE(ASYNC_FLUSH, async->uid, expires);
    uint64_t now = async_now(async);
    while (now < expires) {
        uint64_t next_timeout;
        int status = async_poll(async, &next_timeout);
        if (status < 0) {
            FSTRACE(ASYNC_FLUSH_FAIL, async->uid);
            return -1;
        }
        now = async_now(async);
        if (next_timeout > now) {
            FSTRACE(ASYNC_FLUSH_TIMERS_PENDING, async->uid);
            return 0;
        }
    }
    FSTRACE(ASYNC_FLUSH_EXPIRED, async->uid);
#ifdef ETIME
    errno = ETIME;
#else
    errno = ETIMEDOUT;
#endif
    return -1;
}

FSTRACE_DECL(ASYNC_LOOP_NO_TIMERS, "UID=%64u");
FSTRACE_DECL(ASYNC_LOOP_NEXT_TIMER, "UID=%64u EXPIRES=%64u");
FSTRACE_DECL(ASYNC_LOOP_TIMEOUT, "UID=%64u OBJ=%p ACT=%p");

/* Return nanoseconds till the next timer expiry or a negative number. */
static int64_t take_immediate_action(async_t *async)
{
    enum {
        MAX_IO_STARVATION = 20,
    };
    int i;
    for (i = 0; !async->quit && i < MAX_IO_STARVATION; i++) {
        async_timer_t *timer = earliest_timer(async);
        if (timer == NULL) {
            FSTRACE(ASYNC_LOOP_NO_TIMERS, async->uid);
            return -1;
        }
        int64_t ns = ns_remaining(async, timer);
        if (ns > 0) {
            FSTRACE(ASYNC_LOOP_NEXT_TIMER, async->uid, timer->expires);
            return ns;
        }
        action_1 action = timer->action;
        FSTRACE(ASYNC_LOOP_TIMEOUT, timer->seqno, timer->action.obj,
                timer->action.act);
        if (FSTRACE_ENABLED(ASYNC_TIMER_BT) && timer->stack_trace)
            emit_timer_backtrace(timer);
        timer_cancel(async, timer);
        action_1_perf(action);
    }
    return 0;
}

#if USE_EPOLL
static int ns_to_ms(int64_t ns)
{
    if (ns < 0)
        return -1;
    if (ns > INT_MAX * 1000000LL)
        return INT_MAX;
    /* Rounding up is the right thing to do for timeouts to prevent
     * spurious wakeups. */
    return (ns + 999999) / 1000000;
}
#else
static struct timespec *ns_to_timespec(int64_t ns, struct timespec *t)
{
    if (ns < 0)
        return NULL;
    t->tv_sec = ns / 1000000000;
    t->tv_nsec = ns % 1000000000;
    return t;
}
#endif

FSTRACE_DECL(ASYNC_LOOP, "UID=%64u");
FSTRACE_DECL(ASYNC_LOOP_QUIT, "UID=%64u");
FSTRACE_DECL(ASYNC_LOOP_WAIT, "UID=%64u DELAY-NS=%64u");
FSTRACE_DECL(ASYNC_LOOP_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_LOOP_EXECUTE, "UID=%64u EVENT=%64u");

int async_loop(async_t *async)
{
    FSTRACE(ASYNC_LOOP, async->uid);
    enum {
        MAX_IO_BURST = 20,
    };
    async->wakeup_fd = -1;
    async->quit = false;
    for (;;) {
        int64_t ns = take_immediate_action(async);
        if (async->quit) {
            FSTRACE(ASYNC_LOOP_QUIT, async->uid);
            return 0;
        }
        FSTRACE(ASYNC_LOOP_WAIT, async->uid, ns);
#if USE_EPOLL
        struct epoll_event epoll_events[MAX_IO_BURST];
        int count = epoll_wait(async->poll_fd, epoll_events, MAX_IO_BURST,
                               ns_to_ms(ns));
#else
        struct kevent kq_events[MAX_IO_BURST];
        struct timespec t;
        int count = kevent(async->poll_fd, NULL, 0, kq_events, MAX_IO_BURST,
                           ns_to_timespec(ns, &t));
#endif
        if (count < 0) {
            FSTRACE(ASYNC_LOOP_FAIL, async->uid);
            return -1;
        }
        int i;
        for (i = 0; i < count; i++) {
#if USE_EPOLL
            async_event_t *event = epoll_events[i].data.ptr;
#else
            async_event_t *event = kq_events[i].udata;
#endif
            async_event_trigger(event);
            FSTRACE(ASYNC_LOOP_EXECUTE, async->uid, event->uid);
        }
    }
}

static void drain(int fd)
{
    uint8_t buffer[1024];
    while (read(fd, buffer, sizeof buffer) > 0)
        ;
}

FSTRACE_DECL(ASYNC_LOOP_PROTECTED_PIPE_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_LOOP_PROTECTED, "UID=%64u WAKEUP-RDFD=%d WAKEUP-WRFD=%d");

static bool prepare_protected_loop(async_t *async, int pipefds[2])
{
    if (!unixkit_pipe(pipefds)) {
        FSTRACE(ASYNC_LOOP_PROTECTED_PIPE_FAIL, async->uid);
        return false;
    }
    FSTRACE(ASYNC_LOOP_PROTECTED, async->uid, pipefds[0], pipefds[1]);
    async_register(async, pipefds[0], NULL_ACTION_1);
    nonblock(pipefds[1]);
    async->wakeup_fd = pipefds[1];
    async->quit = false;
    return true;
}

static void finish_protected_loop(async_t *async, int pipefds[2])
{
    async_unregister(async, pipefds[0]);
    close(pipefds[0]);
    close(pipefds[1]);
    async->wakeup_fd = -1;
}

FSTRACE_DECL(ASYNC_LOOP_PROTECTED_WAIT, "UID=%64u DELAY-NS=%64u");
FSTRACE_DECL(ASYNC_LOOP_PROTECTED_FAIL, "UID=%64u ERRNO=%e");
FSTRACE_DECL(ASYNC_LOOP_PROTECTED_EXECUTE, "UID=%64u EVENT=%64u");
FSTRACE_DECL(ASYNC_LOOP_PROTECTED_QUIT, "UID=%64u");

int async_loop_protected(async_t *async, void (*lock)(void *),
                         void (*unlock)(void *), void *lock_data)
{
    enum {
        MAX_IO_BURST = 20,
    };
    int pipefds[2];
    if (!prepare_protected_loop(async, pipefds))
        return -1;
    for (;;) {
        drain(pipefds[0]);
        int64_t ns = take_immediate_action(async);
        if (async->quit) {
            finish_protected_loop(async, pipefds);
            FSTRACE(ASYNC_LOOP_PROTECTED_QUIT, async->uid);
            return 0;
        }
        FSTRACE(ASYNC_LOOP_PROTECTED_WAIT, async->uid, ns);
#if USE_EPOLL
        struct epoll_event epoll_events[MAX_IO_BURST];
        unlock(lock_data);
        int count = epoll_wait(async->poll_fd, epoll_events, MAX_IO_BURST,
                               ns_to_ms(ns));
#else
        struct kevent kq_events[MAX_IO_BURST];
        struct timespec t;
        unlock(lock_data);
        int count = kevent(async->poll_fd, NULL, 0, kq_events, MAX_IO_BURST,
                           ns_to_timespec(ns, &t));
#endif
        int err = errno;
        lock(lock_data);
        if (count < 0) {
            finish_protected_loop(async, pipefds);
            errno = err;
            FSTRACE(ASYNC_LOOP_PROTECTED_FAIL, async->uid);
            return -1;
        }
        int i;
        for (i = 0; i < count; i++) {
#if USE_EPOLL
            async_event_t *event = epoll_events[i].data.ptr;
#else
            async_event_t *event = kq_events[i].udata;
#endif
            async_event_trigger(event);
            FSTRACE(ASYNC_LOOP_PROTECTED_EXECUTE, async->uid, event->uid);
        }
    }
}

FSTRACE_DECL(ASYNC_REGISTER_NONBLOCK_FAIL,
             "UID=%64u FD=%d OBJ=%p ACT=%p ERRNO=%e");
FSTRACE_DECL(ASYNC_REGISTER_FAIL, "UID=%64u FD=%d OBJ=%p ACT=%p ERRNO=%e");
FSTRACE_DECL(ASYNC_REGISTER, "UID=%64u FD=%d OBJ=%p ACT=%p");

int async_register(async_t *async, int fd, action_1 action)
{
    if (nonblock(fd) < 0) {
        FSTRACE(ASYNC_REGISTER_NONBLOCK_FAIL, async->uid, fd, action.obj,
                action.act);
        return -1;
    }
    async_event_t *event = make_async_event(async, action);
#if USE_EPOLL
    struct epoll_event epoll_event;
    epoll_event.events = EPOLLIN | EPOLLOUT | EPOLLET;
    epoll_event.data.ptr = event;
    if (epoll_ctl(async->poll_fd, EPOLL_CTL_ADD, fd, &epoll_event) < 0) {
        FSTRACE(ASYNC_REGISTER_FAIL, async->uid, fd, action.obj, action.act);
        destroy_async_event(event);
        return -1;
    }
#else
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, event);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, event);
    if (kevent(async->poll_fd, changes, 2, NULL, 0, NULL) < 0) {
        FSTRACE(ASYNC_REGISTER_FAIL, async->uid, fd, action.obj, action.act);
        destroy_async_event(event);
        return -1;
    }
#endif
    (void) avl_tree_put(async->registrations, (void *) (intptr_t) fd, event);
    wake_up(async);
    FSTRACE(ASYNC_REGISTER, async->uid, fd, action.obj, action.act);
    return 0;
}

FSTRACE_DECL(ASYNC_REGISTER_OLD_SCHOOL_FAIL,
             "UID=%64u FD=%d OBJ=%p ACT=%p ERRNO=%e");
FSTRACE_DECL(ASYNC_REGISTER_OLD_SCHOOL, "UID=%64u FD=%d OBJ=%p ACT=%p");

int async_register_old_school(async_t *async, int fd, action_1 action)
{
    async_event_t *event = make_async_event(async, action);
#if USE_EPOLL
    struct epoll_event epoll_event;
    epoll_event.events = EPOLLIN;
    epoll_event.data.ptr = event;
    if (epoll_ctl(async->poll_fd, EPOLL_CTL_ADD, fd, &epoll_event) < 0) {
        FSTRACE(ASYNC_REGISTER_OLD_SCHOOL_FAIL, async->uid, fd, action.obj,
                action.act);
        destroy_async_event(event);
        return -1;
    }
#else
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_ADD, 0, 0, event);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD | EV_DISABLE, 0, 0, event);
    if (kevent(async->poll_fd, changes, 2, NULL, 0, NULL) < 0) {
        FSTRACE(ASYNC_REGISTER_OLD_SCHOOL_FAIL, async->uid, fd, action.obj,
                action.act);
        destroy_async_event(event);
        return -1;
    }
#endif
    (void) avl_tree_put(async->registrations, (void *) (intptr_t) fd, event);
    wake_up(async);
    FSTRACE(ASYNC_REGISTER_OLD_SCHOOL, async->uid, fd, action.obj, action.act);
    return 0;
}

FSTRACE_DECL(ASYNC_MODIFY_OLD_SCHOOL_FAIL,
             "UID=%64u FD=%d RD=%d WR=%d ERRNO=%e");
FSTRACE_DECL(ASYNC_MODIFY_OLD_SCHOOL, "UID=%64u FD=%d RD=%d WR=%d");

int async_modify_old_school(async_t *async, int fd, int readable, int writable)
{
    avl_elem_t *elem =
        avl_tree_get(async->registrations, (void *) (intptr_t) fd);
    if (elem == NULL) {
        errno = EBADF;
        return -1;
    }
    async_event_t *event = (async_event_t *) avl_elem_get_value(elem);
#if USE_EPOLL
    struct epoll_event epoll_event;
    epoll_event.events = 0;
    if (readable)
        epoll_event.events |= EPOLLIN;
    if (writable)
        epoll_event.events |= EPOLLOUT;
    epoll_event.data.ptr = event;
    if (epoll_ctl(async->poll_fd, EPOLL_CTL_MOD, fd, &epoll_event) < 0) {
        FSTRACE(ASYNC_MODIFY_OLD_SCHOOL_FAIL, async->uid, fd, readable,
                writable);
        return -1;
    }
#else
    struct kevent changes[2];
    if (readable)
        EV_SET(&changes[0], fd, EVFILT_READ, EV_ENABLE, 0, 0, event);
    else
        EV_SET(&changes[0], fd, EVFILT_READ, EV_DISABLE, 0, 0, event);
    if (writable)
        EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ENABLE, 0, 0, event);
    else
        EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DISABLE, 0, 0, event);
    if (kevent(async->poll_fd, changes, 2, NULL, 0, NULL) < 0)
        return -1;
#endif
    wake_up(async);
    FSTRACE(ASYNC_MODIFY_OLD_SCHOOL, async->uid, fd, readable, writable);
    return 0;
}

FSTRACE_DECL(ASYNC_UNREGISTER_FAIL, "UID=%64u FD=%d ERRNO=%e");
FSTRACE_DECL(ASYNC_UNREGISTER, "UID=%64u FD=%d");

int async_unregister(async_t *async, int fd)
{
#if USE_EPOLL
    if (epoll_ctl(async->poll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        FSTRACE(ASYNC_UNREGISTER_FAIL, async->uid, fd);
        return -1;
    }
#else
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (kevent(async->poll_fd, changes, 2, NULL, 0, NULL) < 0) {
        FSTRACE(ASYNC_UNREGISTER_FAIL, async->uid, fd);
        return -1;
    }
#endif
    avl_elem_t *element =
        avl_tree_pop(async->registrations, (void *) (intptr_t) fd);
    assert(element != NULL);
    destroy_async_event((async_event_t *) avl_elem_get_value(element));
    destroy_avl_element(element);
    FSTRACE(ASYNC_UNREGISTER, async->uid, fd);
    return 0;
}
