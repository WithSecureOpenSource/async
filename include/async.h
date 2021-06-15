#ifndef __ASYNC__
#define __ASYNC__

#include <stdint.h>

#include <fsdyn/fsalloc.h>

#include "action_1.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A simple asynchronous event library.
 *
 * Provides:
 *  * single-threaded epoll-based event multiplexing between socket-like
 *    file descriptors
 *  * timers
 *  * a main loop
 *  * a possibility to integrate with some other main loop
 */

/*
 * You need to allocate and initialize an async_t object to make use of
 * the facilities of this library. Typically, this would be a singleton
 * object.
 *
 * You must not touch the fields of the data structure. Call
 * async_init() to start using an async_t object.
 */
typedef struct async async_t;

/*
 * An opaque timer object.
 */
typedef struct async_timer async_timer_t;

/*
 * An opaque event object. An event is an asynchronous callback that
 * can be triggered or canceled. If it is triggered multiple times
 * between execution, only a single execution takes place.
 */
typedef struct async_event async_event_t;

/*
 * Create an async object.
 *
 * A NULL return value indicates a fatal error (consult errno).
 */
async_t *make_async(void);

/*
 * Destroy an async object.
 */
void destroy_async(async_t *async);

/*
 * Return the current point in time. The return value is an unsigned,
 * non-wrapping, monotonic nanosecond counter. The time base is
 * unspecified and cannot be assumed to be the same across async objects
 * (and systems).
 */
uint64_t async_now(async_t *async);

#define ASYNC_NS   ((int64_t) 1)
#define ASYNC_US   (1000 * ASYNC_NS)
#define ASYNC_MS   (1000 * ASYNC_US)
#define ASYNC_S    (1000 * ASYNC_MS)
#define ASYNC_MIN  (60 * ASYNC_S)
#define ASYNC_H    (60 * ASYNC_MIN)
#define ASYNC_DAY  (24 * ASYNC_H)
#define ASYNC_WEEK (7 * ASYNC_DAY)

/*
 * Start a timer that expires at the given absolute time (expressed in
 * epoch nanoseconds).
 */
async_timer_t *async_timer_start(async_t *async, uint64_t expires,
                                 action_1 action);

/*
 * Cancel a timer. You must not cancel a timer twice. Also, you must not
 * cancel a timer that has already expired.
 */
void async_timer_cancel(async_t *async, async_timer_t *timer);

/*
 * Create an event. The event must be triggered separately after
 * creation.
 */
async_event_t *make_async_event(async_t *async, action_1 action);

/*
 * Trigger an event. The event's action will be executed once from the
 * main loop unless it is canceled before execution.
 */
void async_event_trigger(async_event_t *event);

/*
 * Cancel past triggers. If the event is not triggered, this function
 * has no effect.
 */
void async_event_cancel(async_event_t *event);

/*
 * Cancel and destroy the event.
 */
void destroy_async_event(async_event_t *event);

/*
 * Run a task from the main loop. The action takes place without
 * delay. However, before it is put into effect, it can be canceled
 * using async_timer_cancel().
 */
async_timer_t *async_execute(async_t *async, action_1 action);

/*
 * Deallocate object using fsfree() from the main loop. The caller
 * should incapacitate the object in the meantime so no new references
 * to it are introduced into the system.
 *
 * The function is used as a poor man's garbage collection scheme. An
 * immediate fsfree() call might be dangerous because of references in
 * scheduled tasks. So instead, you should call async_wound() and mark
 * the object as no longer operational.
 */
void async_wound(async_t *async, void *object);

/*
 * Returns a single file descriptor that can be used to integrate this
 * library with some other event framework. Whenever the returned file
 * descriptor becomes readable, the external framework must call
 * async_poll().
 */
int async_fd(async_t *async);

/*
 * If the async object is linked with an external main loop,
 * async_poll() must be called as soon as async_fd() becomes readable.
 * It is ok to call async_poll() spuriously.
 *
 * Returns a negative number in case of an error (consult errno). Some
 * errors may not be fatal.
 *
 * If a nonnegative number is returned, pnext_timeout is used to pass
 * back the latest point in time (in async_now() time frame) that
 * async_poll() must be invoked even if async_fd() didn't become
 * readable. The time (uint64_t) -1 is used to indicate that no such
 * time limit applies. If the time refers to the past, async_poll() must
 * be called again without delay.
 *
 * The function never blocks.
 */
int async_poll(async_t *async, uint64_t *pnext_timeout);

/*
 * This is the native main loop of the async library. The function never
 * returns unless async_quit_loop() is called or a system call error is
 * encountered.
 *
 * Returns a negative number in case of an error (consult errno). Some
 * errors (eg, EINTR) may not be fatal.
 *
 * These functions must not be called while async_loop() is in execution:
 *  - async_loop()
 *  - async_loop_protected()
 *  - async_poll()
 *  - destroy_async()
 */
int async_loop(async_t *async);

/*
 * This function works like async_loop() (qv). Additionally, the two
 * given callbacks are invoked around a critical section, making it
 * possible to integrate the main loop safely with other threads.
 *
 * The caller must call async_loop_protected in the locked state;
 * async_loop_protected returns in the locked state as well. Whenever it
 * enters a safe spot, async_loop_protected calls unlock(lock_data). On
 * leaving the safe spot, it calls lock(lock_data).
 *
 * When this function is used, other async library functions can be
 * called from extraneous threads as long as the function call is
 * enclosed in a critical section (with lock() and unlock()). However,
 *
 * These functions must not be called while async_loop_protected() is in
 * execution:
 *  - async_loop()
 *  - async_loop_protected()
 *  - async_poll()
 *  - destroy_async()
 */
int async_loop_protected(async_t *async, void (*lock)(void *),
                         void (*unlock)(void *), void *lock_data);

/*
 * This function causes async_loop() or async_loop_protected() to return
 * after processing the current event. It can be called safely from a
 * signal handler or a separate thread.
 */
void async_quit_loop(async_t *async);

/*
 * Loop until all immediately pending events have been processed or
 * the specified expiry time has been reached.
 *
 * A negative return value indicates an error (consult errno).
 *
 * if flushing couldn't complete in the given time, errno is set to
 * ETIME, if defined, and to ETIMEDOUT otherwise.
 */
int async_flush(async_t *async, uint64_t expires);

/*
 * Start monitoring the given socket-like file descriptor for change of
 * status. Whenever the file descriptor becomes readable or writable,
 * the task is run from the main loop.
 *
 * The callback is guaranteed only after EAGAIN has been returned by a
 * previous read or write operation on the file descriptor.
 *
 * As a side effect, async_register() makes the file descriptor
 * nonblocking.
 *
 * A negative return value indicates an error (consult errno).
 */
int async_register(async_t *async, int fd, action_1 action);

/*
 * Start monitoring the given socket-like file descriptor for change of
 * status. Whenever the file descriptor is readable, the task is run
 * from the main loop. Unless input from the file descriptor is
 * exhausted, the action is taken again immediately.
 *
 * The file descriptor's blocking behavior is not touched.
 *
 * A negative return value indicates an error (consult errno).
 */
int async_register_old_school(async_t *async, int fd, action_1 action);

/*
 * Right after async_register_old_school(), the file descriptor is
 * monitored for readability. This function can be used to turn on or
 * off readability and writability monitoring.
 *
 * A negative return value indicates an error (consult errno).
 */
int async_modify_old_school(async_t *async, int fd, int readable, int writable);

/*
 * Stop monitoring a file descriptor.
 *
 * A negative return value indicates an error (consult errno).
 */
int async_unregister(async_t *async, int fd);

#ifdef __cplusplus
}

#include <functional>
#include <memory>

#if __cplusplus >= 201703L
#include <variant>
#endif

namespace fsecure {
namespace async {

// std::unique_ptr for async_t with custom deleter.
using AsyncPtr = std::unique_ptr<async_t, std::function<void(async_t *)>>;

// Create AsyncPtr that takes ownership of the provided async_t. Pass nullptr to
// create an instance which doesn't contain any async_t object.
inline AsyncPtr make_async_ptr(async_t *async)
{
    return { async, destroy_async };
}

// Create AsyncPtr which owns a newly created async_t object.
inline AsyncPtr make_async_ptr()
{
    return { make_async(), destroy_async };
}

/*
 * This object provides an indirection layer for objects that use
 * async with the purpose of ensuring that once an object is destroyed
 * no reference to it is present in the associated async loop.
 */
#if __cplusplus >= 201703L
class ProxyVariant {
    using function_callback_t = std::function<void(void)>;
    struct Data {
        async_t *async;
        std::variant<action_1, function_callback_t> action;
    };

    static void destroy_data(Data *data) { delete data; }

public:
    ProxyVariant(async_t *async, action_1 action)
        : ProxyVariant(async)
    {
        data_->action = action;
    }

    ProxyVariant(async_t *async, function_callback_t action)
        : ProxyVariant(async)
    {
        data_->action = action;
    }

    virtual ~ProxyVariant()
    {
        data_->action = NULL_ACTION_1;
        async_execute(data_->async, (action_1) { data_, (act_1) destroy_data });
    }

    ProxyVariant(const ProxyVariant &) = delete;
    ProxyVariant(ProxyVariant &&) = delete;
    ProxyVariant &operator=(const ProxyVariant &) = delete;
    ProxyVariant &operator=(ProxyVariant &&) = delete;

    action_1 get_callback() { return { data_, (act_1) action_cb }; }

private:
    ProxyVariant(async_t *async)
    {
        data_ = new Data();
        data_->async = async;
    }

    Data *data_;

    // helper type for the static_assert in action_cb
    template <class T>
    struct always_false : std::false_type {};

    static void action_cb(Data *data)
    {
        std::visit(
            [](auto &action) {
                using T = std::decay_t<decltype(action)>;
                if constexpr (std::is_same_v<T, action_1>)
                    action_1_perf(action);
                else if constexpr (std::is_same_v<T, function_callback_t>)
                    action();
                else
                    static_assert(always_false<T>::value,
                                  "non-exhaustive visitor!");
            },
            data->action);
    }
};
#endif // #if __cplusplus >= 201703L

class Proxy {
    struct Data {
        async_t *async;
        action_1 action;
    };

public:
    Proxy(async_t *async, action_1 action)
    {
        data_ = (Data *) fsalloc(sizeof *data_);
        data_->async = async;
        data_->action = action;
    }

    virtual ~Proxy()
    {
        data_->action = NULL_ACTION_1;
        async_wound(data_->async, data_);
    }

    Proxy(const Proxy &) = delete;
    Proxy(Proxy &&) = delete;
    Proxy &operator=(const Proxy &) = delete;
    Proxy &operator=(Proxy &&) = delete;

    action_1 get_callback() { return { data_, (act_1) action_cb }; }

private:
    Data *data_;

    static void action_cb(Data *data) { action_1_perf(data->action); }
};

} // namespace async
} // namespace fsecure
#endif

#endif
