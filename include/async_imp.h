struct async {
    uint64_t uid;
    int poll_fd;
    avl_tree_t *timers;
    avl_tree_t *registrations;
    volatile bool quit;
    int wakeup_fd;
    list_t *wounded_objects;
    uint64_t recent;
#ifdef __MACH__
    clock_serv_t mach_clock;
#endif
};

typedef struct {
    uint64_t expires;
    uint64_t seqno;
} async_timer_key_t;

struct async_timer {
    async_timer_key_t key;
    action_1 action;
    void **stack_trace;      /* Where the timer was scheduled or NULL */
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
    void **stack_trace;      /* Where the timer was scheduled or NULL */
};
