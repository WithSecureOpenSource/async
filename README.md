## Overview

async is a C library for Linux and macOS that provides:
- a single-threaded main loop that dispatches callback events
- edge-triggered file-descriptor monitoring (using Linux `epoll(2)` or BSD
  `kqueue(2)` system calls)
- timers
- TCP client and server connections
- other useful byte streams

## Building

async uses [SCons][] and `pkg-config` for building and depends on the following
libraries:
- [encjson][]
- [fsdyn][]
- [fstrace][]
- [unixkit][]

To build async, run
```
scons prefix=<prefix> install
```
from the top-level async directory. The prefix argument is a directory,
`/usr/local` by default, where the build system searches for `async`
dependencies and installs `async`.

## The Structure of an Async Application

Here is the skeleton of a simple `async` application:
```
#include <async/async.h>

typedef struct {
    async_t *async;
    int fd;
    /*...*/
} my_app_t;

static void process_event(my_app_t *app)
{
    char buf[100];
    ssize_t count = read(app->fd, buf, sizeof buf);
    /*...*/
}

static void initialize(my_app_t *app)
{
    app->fd = my_channel_open(/*...*/);
    action_1 callback = { app, (act_1) process_event };
    int status = async_register(app->async, app->fd, callback);
    if (status < 0) {
        /* see errno */
    }
    async_execute(app->async, callback);    /* the first time's on us */
}

int main(void)
{
    my_app_t app;
    app.async = make_async();
    initialize(&app);
    async_loop(app.async);
    destroy_async(app.async);   /* if you want */
    return 0;
}
```
Notes:
- A single `async` object is created for the lifetime of the process.
- The `async_loop()` function blocks "for ever" (use `async_quit_loop()` to exit
  the loop).
- One or more file descriptors are handed to `async` with `async_register()`.
  Only socket-like ("selectable") file descriptors are suitable.
- Registration makes the file descriptor nonblocking as a side effect.
- Event handlers must never sleep or block but must return ASAP. Use state
  machines to manage control flow.
- A file descriptor is only guaranteed a notification when its I/O state changes
  after registration. Initially, it is considered readable and writable. If
  `read()`, `recv*()`, `accept()`, `write()`, `send*()` or `connect()` returns
  with `EAGAIN` or `EINPROGRESS`, the I/O state changes and the application can
  rely on a callback as soon as the file operation should be tried again. That
  is why the sample application above uses `async_execute()` to schedule the
  callback during initialization.
- The application is responsible for exhausting its inputs before a callback is
  guaranteed. However, care must be taken so the application does not spin in a
  loop servicing one file descriptor for ever. At times, control should be
  relinquished to other activities. Resume reading the input by calling
  `async_execute()` before returning from the callback.
- Callbacks may be spurious.
- `async_loop()` returns a negative integer (with `errno` set) in case of an I/O
  error. In particular, a signal will make `async_loop()` return with `EINTR`.

## Timers and Tasks
You start a timer with
```
async_timer_t *my_timer =
    async_timer_start(app->async, async_now(app->async) + 5 * ASYNC_S,
    (action_1) { app, (act_1) callback });
```
which causes `callback(app)` to be called by `async` 5 seconds from now. Note
that the expiry is expressed as a point in time instead of a delay;
`async_now()` returns an unsigned, non-wrapping, ever-increasing nanosecond
counter. If it refers to the past, the timer expires immediately—however, the
callback is always invoked from `async_loop()` and not from
`async_timer_start()`.

A running timer can be canceled with `async_timer_cancel(async, timer)`.
Canceling a non-running timer is a sure way to crash the process.

If you want a task executed right away, call `async_execute(async, callback)`. A
direct function call is more immediate, of course, but "backgrounding" tasks
often has benefits: you can complete state transitions before the scheduled
action is taken and multiple activities are interleaved fairly by the main loop.

Tasks scheduled with `async_execute()` cannot be canceled.

## Notifications

The `<async/notification.h>` module provides a notification object that wraps an
action and can safely schedule it from a signal handler or separate thread. You
create a notification with
```
notification_t *my_notification =
    make_notification(app->async, (action_1) { app, (act_1) callback });
```
The notification can be issued at any time with
`issue_notification(my_notification)` and can be destroyed with
`destroy_notification(my_notification)`.

## Byte Streams and Yields
`async` includes a collection of byte stream and yield types. The types are
implemented in C++'esque C which allows for interfaces and virtual functions.

A byte stream type implements the `<async/bytestream_1.h>` interface. That is,
any byte stream object can be "typecast" into a `bytestream_1` object with a
function. Thus, a chunk encoder object is converted into a `bytestream_1` object
with `chunkencoder_as_bytestream_1(encoder)` and a pacer stream object is
converted with `pacerstream_as_bytestream_1(pacer)`.

(Interface types sport a numeric tag. An interface never changes. If a change is
needed, the numeric tag is incremented, and both versions of the interface
remain valid.)

The `bytestream_1` interface has four methods:
- `ssize_t read(void *stream, void *buf, size_t count)`

- `void close(void *stream)`

- `void register_callback(void *stream, action_1 action)`

- `void unregister_callback(void *stream)`

Each byte stream type additionally contains a constructor to create an object of
the type, and the `bytestream_1` methods as functions prefixed with the type
name.

The semantics of the methods is mostly obvious. The `close()` function acts as a
destructor for the object. (Closed stream objects become inactive immediately
and are deallocated by the main loop.)

Byte streams are often chained much like Unix pipelines for similarly diverse
effects.

A yield is a sequence of arbitrary data objects, typically driven by I/O events.
A yield type implements the `<async/yield_1.h>` interface. That is, any yield
object can be "typecast" into a `yield_1` object with a function.

Similarly to the byte stream interface, the `yield_1` interface has four
methods:
- `void *receive(void *yield)`

- `void close(void *yield)`

- `void register_callback(void *yield, action_1 action)`

- `void unregister_callback(void *yield)`

The `receive` method returns an object whose type depends on the specific yield.
Each yield type additionally contains a constructor to create an object of the
type, and the `yield_1` methods as functions prefixed with the type name.

A yield is usually implemented on top of a byte stream that decodes a single
object out of a source stream.

### Base64 encoder
`<async/base64encoder.h>`

A byte stream that encodes another stream in Base64 encoding.

### Base64 decoder
`<async/base64decoder.h>`

A byte stream that decodes another stream in Base64 encoding.

### Blob stream
 `<async/blobstream.h>`

A byte array delivered as a byte stream.

### Blocking stream
`<async/blockingstream.h>`

Any blocking file descriptor (e.g., a regular file) dressed as a byte stream;
use with care.

### Chunk encoder
`<async/chunkencoder.h>`

A byte stream that encodes another stream in the chunked transfer encoding
format.

### Chunk decoder
`<async/chunkdecoder.h>`

A byte stream that decodes another stream in the chunked transfer encoding
format.

### Clobber stream

`<async/clobberstream.h>`

A byte stream that corrupts another stream.

### Concat stream
`<async/concatstream.h>`

A byte stream that concatenates a fixed number of streams into one (like cat).

### Dry stream
`<async/drystream.h>`

A byte stream that always returns `EAGAIN`.

### Empty stream
`<async/emptystream.h>`

A byte stream that gives out an immediate EOF (like `/dev/null`).

### Error stream
`<async/errorstream.h>`

A byte stream that always returns a given error.

### Farewell stream
`<async/farewellstream.h>`

A byte stream that wraps another stream and invokes a callback as soon as
`close()` is called on it.

### Iconv stream
`<async/iconvstream.h>`

A byte stream that converts a character stream from one character encoding to
another.

### JSON encoder
`<async/jsonencoder.h>`

A byte stream that encodes a JSON object.

### Multipart decoder
`<async/multipartdecoder.h>`

A byte stream that decodes one part of a RFC 2046 multipart body stream.

### Naive encoder
`async/naiveencoder.h>`

A byte stream that encodes another stream by appending a terminator byte and
optionally escaping special bytes.

### Naive decoder
`async/naivedecoder.h>`

A byte stream that decodes another stream in the "naive" encoding.

### Nice stream
`<async/nicestream.h>`

A byte stream that wraps another stream and occasionally returns an `EAGAIN` if
the stream seems to be readable for ever.

### Pacer stream
`<async/pacerstream.h>`

A byte stream that wraps another stream and gives out bytes at a steady rate.
Allows for an implementation of periodic timers (every read byte corresponds to
a timer tick).

### Pause stream
`<async/pausestream.h>`

A blocking stream that returns `EAGAIN` when a given number of bytes has been
read. Reading can be resumed by raising the limit.

### Pipe stream
`<async/pipestream.h>`

Any nonblocking file descriptor dressed as a byte stream.

### Probe stream
`<async/probestream.h>`

A byte stream that can be used to trace reading and closing activities of
another stream.

### Queue stream
`<async/queuestream.h>`

A byte stream that concatenates streams dynamically on the fly.

### String stream
`<async/stringstream.h>`

A C string delivered as a byte stream.

### Substream
`<async/substream.h>`

A byte stream that delivers a portion of another stream (like head and tail).

### Switch stream
`<async/switchstream.h>`

A byte stream that wraps another stream and can switch to a different one on the
fly.

### Trickle stream
`<async/tricklestream.h>`

A byte stream that slows down the transmission of another stream to a single
byte per a given interval.

### Zero stream
`<async/zerostream.h>`

A byte stream that delivers a never-ending supply of zero bytes (like
`/dev/zero`).

## JSON yield
`<async/jsonyield.h>`

A yield of JSON objects delimited with the NUL byte using ESC as the escape
character. The yield input is a `bytestream_1` object and the outputs are
`json_thing_t` objects.

## Multipart deserializer
`<async/multipartdeserializer.h>`

A yield of the parts of a RFC 2046 multipart body. The yield input is a
`bytestream_1` object and the outputs are `bytestream_1` objects.

### Naive framer
`<async/naiveframer.h>`

A yield of frames delimited with "naive" encoding. The yield input is a
`bytestream_1` object and the outputs are `bytestream_1` objects.

## JSON decoder

The `<async/jsondecoder.h>` module provides an object that decodes a single JSON
object from a byte stream. As opposed to other decoders, the JSON decoder is not
a byte stream.

## TCP
The `<async/tcp_connection.h>` module integrates TCP (and Unix stream socket)
connections with `async` and the byte stream mechanism. A TCP connection is
presented as a pair of byte streams. The TCP connection supplies a byte stream
for receiving bytes from the socket. The application must supply a byte stream
for sending bytes to the socket. Thus, bytes are only read, never written. A
typical usage pattern is to supply the TCP connection with an outbound queue
stream and send messages with the `queuestream_enqueue()` function.

To create a TCP (or Unix stream socket) client, call `tcp_connect()`,
`tcp_register_callback()` and `tcp_set_output_stream()`.

To create a TCP (or Unix stream socket) server, call `tcp_listen()`,
`tcp_register_server_callback()` and `tcp_accept()`.

To create a socket pair, call `socketpair()` followed by
`tcp_adopt_connection()`.

Note that `tcp_close()` closes both associated byte streams. You can close the
byte streams individually using the `bytestream_1` facilities, or, use the
`tcp_shut_down()` method. Even after both byte streams have been closed/shut
down, `tcp_close()` needs to be called to free up the resources.

[SCons]: https://scons.org/
[encjson]: https://github.com/F-Secure/encjson
[fsdyn]: https://github.com/F-Secure/fsdyn
[fstrace]: https://github.com/F-Secure/fstrace
[unixkit]: https://github.com/F-Secure/unixkit
