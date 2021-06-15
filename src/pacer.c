#include "pacer.h"

#include <assert.h>

#include <fsdyn/fsalloc.h>
#include <fsdyn/list.h>
#include <fstrace.h>

#include "async_version.h"

struct pacer {
    async_t *async;
    uint64_t uid;
    double rate, initial, maximum;
    uint64_t start_time;
    async_timer_t *timer;
    list_t *queue;
};

struct pacer_ticket {
    pacer_t *pacer;
    uint64_t uid;
    double limit, debit;
    action_1 probe;
    list_elem_t *iter;
};

FSTRACE_DECL(ASYNC_PACER_CREATE,
             "UID=%64u PTR=%p ASYNC=%p RATE=%f INIT=%f MAX=%f START=%64u");

pacer_t *make_pacer(async_t *async, double rate, double initial, double maximum,
                    uint64_t start_time)
{
    pacer_t *pacer = fsalloc(sizeof *pacer);
    pacer->async = async;
    pacer->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_PACER_CREATE, pacer->uid, pacer, async, rate, initial,
            maximum, start_time);
    pacer->rate = rate;
    pacer->initial = initial;
    pacer->maximum = maximum;
    pacer->start_time = start_time;
    pacer->timer = NULL;
    pacer->queue = make_list();
    return pacer;
}

FSTRACE_DECL(ASYNC_PACER_DESTROY, "UID=%64u");

void destroy_pacer(pacer_t *pacer)
{
    FSTRACE(ASYNC_PACER_DESTROY, pacer->uid);
    assert(pacer->async != NULL);
    if (pacer->timer)
        async_timer_cancel(pacer->async, pacer->timer);
    while (!list_empty(pacer->queue))
        fsfree((void *) list_pop_first(pacer->queue));
    destroy_list(pacer->queue);
    async_wound(pacer->async, pacer);
    pacer->async = NULL;
}

static double calc_available(pacer_t *pacer, uint64_t t)
{
    int64_t age = t - pacer->start_time;
    double amount = pacer->initial + age / (double) ASYNC_S * pacer->rate;
    if (amount > pacer->maximum)
        return pacer->maximum;
    return amount;
}

static void pacer_probe(pacer_t *pacer);

static void start_timer(pacer_ticket_t *ticket, double amount, uint64_t now)
{
    pacer_t *pacer = ticket->pacer;
    const double MAX_WAIT = 100000; /* guard against uint64_t overflow */
    double time_to_wait;
    if (pacer->rate <= 0)
        time_to_wait = MAX_WAIT;
    else {
        time_to_wait = (ticket->limit - amount) / pacer->rate;
        if (time_to_wait > MAX_WAIT)
            time_to_wait = MAX_WAIT;
    }
    if (time_to_wait < 0) /* guard against uint64_t underflow */
        time_to_wait = 0;
    pacer->timer = async_timer_start(pacer->async,
                                     now + (uint64_t)(time_to_wait * ASYNC_S),
                                     (action_1) { pacer, (act_1) pacer_probe });
}

FSTRACE_DECL(ASYNC_PACER_PROBE, "UID=%64u");
FSTRACE_DECL(ASYNC_PACER_TRIGGER, "UID=%64u TICKET=%64u OBJ=%p ACT=%p");
FSTRACE_DECL(ASYNC_PACER_PROBED, "UID=%64u");

static void pacer_probe(pacer_t *pacer)
{
    FSTRACE(ASYNC_PACER_PROBE, pacer->uid);
    do {
        assert(!list_empty(pacer->queue));
        pacer_ticket_t *ticket = (void *) list_pop_first(pacer->queue);
        uint64_t now = async_now(pacer->async);
        double amount = calc_available(pacer, now);
        if (amount < ticket->limit) {
            ticket->iter = list_prepend(pacer->queue, ticket);
            start_timer(ticket, amount, now);
            break;
        }
        pacer->timer = NULL;
        FSTRACE(ASYNC_PACER_TRIGGER, pacer->uid, ticket->uid, ticket->probe.obj,
                ticket->probe.act);
        action_1_perf(ticket->probe); /* typically calls pacer_get() */
        fsfree(ticket);
    } while (pacer->timer == NULL && !list_empty(pacer->queue));
    FSTRACE(ASYNC_PACER_PROBED, pacer->uid);
}

FSTRACE_DECL(ASYNC_PACER_GRANTED,
             "UID=%64u NOW=%64u AMOUNT=%f LIMIT=%f DEBIT=%f LEFT=%f");
FSTRACE_DECL(ASYNC_PACER_ENQUEUED,
             "UID=%64u TICKET=%64u NOW=%64u AMOUNT=%f LIMIT=%f DEBIT=%f "
             "OBJ=%p ACT=%p");

pacer_ticket_t *pacer_get(pacer_t *pacer, double limit, double debit,
                          action_1 probe)
{
    uint64_t now = async_now(pacer->async);
    double amount = calc_available(pacer, now);
    if (amount >= limit) {
        pacer->initial = amount - debit;
        FSTRACE(ASYNC_PACER_GRANTED, pacer->uid, now, amount, limit, debit,
                pacer->initial);
        pacer->start_time = now;
        return NULL;
    }
    pacer_ticket_t *ticket = fsalloc(sizeof *ticket);
    ticket->pacer = pacer;
    ticket->uid = fstrace_get_unique_id();
    FSTRACE(ASYNC_PACER_ENQUEUED, pacer->uid, ticket->uid, now, amount, limit,
            debit, probe.obj, probe.act);
    ticket->limit = limit;
    ticket->debit = debit;
    ticket->probe = probe;
    ticket->iter = list_append(pacer->queue, ticket);
    if (pacer->timer == NULL)
        start_timer(ticket, amount, now);
    return ticket;
}

FSTRACE_DECL(ASYNC_PACER_CANCEL, "UID=%64u TICKET=%64u");

void pacer_cancel(pacer_ticket_t *ticket)
{
    pacer_t *pacer = ticket->pacer;
    FSTRACE(ASYNC_PACER_CANCEL, pacer->uid, ticket->uid);
    if (list_get_first(pacer->queue) == ticket->iter) {
        assert(pacer->timer != NULL);
        async_timer_cancel(pacer->async, pacer->timer);
        pacer->timer = NULL;
    }
    list_remove(pacer->queue, ticket->iter);
    fsfree(ticket);
    if (pacer->timer == NULL && !list_empty(pacer->queue))
        async_execute(pacer->async, (action_1) { pacer, (act_1) pacer_probe });
}

double pacer_available(pacer_t *pacer)
{
    return calc_available(pacer, async_now(pacer->async));
}

double pacer_backlog(pacer_t *pacer, unsigned *ticket_count)
{
    *ticket_count = list_size(pacer->queue);
    double backlog = 0;
    list_elem_t *e;
    for (e = list_get_first(pacer->queue); e; e = list_next(e)) {
        pacer_ticket_t *ticket = (pacer_ticket_t *) list_elem_get_value(e);
        backlog += ticket->debit;
    }
    return backlog;
}
