struct async {
    uint64_t uid;
    int poll_fd;
    list_t *immediate; /* of async_timer_t */
    priorq_t *timers;
    avl_tree_t *registrations;
    volatile bool quit;
    int wakeup_fd;
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
