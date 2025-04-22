#pragma once

#ifdef __linux__
#ifdef NO_TIMERFD
#define PIPE_WAKEUP NO_TIMERFD
#else
#define PIPE_WAKEUP 0
#endif
#endif

#include <fsdyn/avltree.h>
#include <fsdyn/list.h>
#include <fsdyn/priority_queue.h>

#include "async.h"

struct async {
    uint64_t uid;
    int poll_fd;
    list_t *immediate; /* of async_timer_t */
    priorq_t *timers;
    avl_tree_t *registrations;
    volatile bool quit;
#ifdef __linux__
    int wakeup_fd;
#if PIPE_WAKEUP
    int wakeup_trigger_fd;
#endif
#else
    bool wakeup_needed;
#endif
    list_t *wounded_objects;
    uint64_t recent;
#ifdef __MACH__
    clock_serv_t mach_clock;
#endif
};

struct async_timer {
    uint64_t expires;
    uint64_t seqno;
    bool immediate;
    void *loc;
    action_1 action;
    void **stack_trace; /* Where the timer was scheduled or NULL */
};

typedef enum {
    ASYNC_EVENT_IDLE,
    ASYNC_EVENT_TRIGGERED,
    ASYNC_EVENT_CANCELED,
    ASYNC_EVENT_ZOMBIE
} async_event_state_t;

struct async_event {
    async_t *async;
    uint64_t uid;
    async_event_state_t state;
    action_1 action;
    void **stack_trace; /* Where the timer was scheduled or NULL */
};

int async_nonblock(int fd);
int async_register_event(async_t *async, int fd, async_event_t *event);

/* The wakeup mechanism registers a dummy event. */
#define ASYNC_SENTINEL_EVENT ((async_event_t *) NULL)

/*
 * Operating-system-dependent functions that implement async's timer
 * wakeup mechanism. Note that the wakeup mechanism is not needed when
 * async_loop() is used as it can manage with a calculated epoll or
 * kqueue timeout. It is used with async_poll(), async_poll_2() or
 * async_loop_protected(), which need to be prepared for external
 * event loops as well as events from external sources.
 */
void async_initialize_wakeup(async_t *async);
void async_cancel_wakeup(async_t *async);
void async_dismantle_wakeup(async_t *async);
void async_wake_up(async_t *async);
void async_schedule_wakeup(async_t *async, uint64_t expires);
void async_arm_wakeup(async_t *async);
bool async_set_up_wakeup(async_t *async);
