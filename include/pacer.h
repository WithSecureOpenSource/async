#ifndef __PACER__
#define __PACER__

#include "action_1.h"
#include "async.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pacer pacer_t;
typedef struct pacer_ticket pacer_ticket_t;

/*
 * A pacer object can be used to implement constant rates. The internal
 * value of the pacer continuously increases at the specified rate. The
 * start_time is an absolute async timestamp and may be before or after
 * async_now().
 */
pacer_t *make_pacer(async_t *async, double rate, double initial, double maximum,
                    uint64_t start_time);

/*
 * A pacer is disposed of with this function.
 */
void destroy_pacer(pacer_t *pacer);

/*
 * If the pacer's internal value is greater than or equal to limit, the
 * given debit amount is subtracted from it and NULL is returned.
 *
 * If the internal value is less than limit, a non-NULL ticket is
 * returned (see pacer_cancel()). The given probe callback will be
 * called later to let the caller know when pacer_get should be tried
 * again.
 *
 * It is advisable to invoke pacer_get() before returning from probe so
 * the caller's place in the queue is not lost.
 *
 * A single pacer can take more than one pacer_get request in the queue.
 * They are served in the order the calls are made (FIFO).
 *
 * Note: limit = 0 and limit = debit are the typical values.
 */
pacer_ticket_t *pacer_get(pacer_t *pacer, double limit, double debit,
                          action_1 probe);

/*
 * Use pacer_cancel() should you need to cancel the ticket returned by
 * pacer_get(). The ticket is valid until the moment the probe callback
 * is invoked. Calling pacer_cancel() guarantees the probe callback will
 * not be invoked.
 */
void pacer_cancel(pacer_ticket_t *ticket);

/*
 * The current internal value of the pacer is returned.
 */
double pacer_available(pacer_t *pacer);

/*
 * Return the sum of debits in the outstanding tickets. The number of
 * outstanding tickets is relayed in ticket_count.
 */
double pacer_backlog(pacer_t *pacer, unsigned *ticket_count);

#ifdef __cplusplus
}
#endif

#endif
