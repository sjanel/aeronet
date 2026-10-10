# Servers and lifecycle

aeronet has two server classes. Both own their configuration, router, sockets, and event loops: there is no global state, so several servers can coexist in one process.

| Class | Threads | Use it when |
| --- | --- | --- |
| `SingleHttpServer` | One event loop | One core is enough, or you manage the threads yourself. |
| `MultiHttpServer`, aliased `HttpServer` | One event loop per thread, `nbThreads` of them (all cores by default) | The usual choice for a production server. |

## Threading model

A `SingleHttpServer` runs one event loop on one thread: it accepts connections, reads requests, calls handlers, and writes responses, without locks. Handlers, middleware, and callbacks all run on that thread, which is why they must not block (see [Async handlers](async-handlers.md) for blocking work).

A `MultiHttpServer` runs `nbThreads` independent `SingleHttpServer` instances on the same port (`0`, the default, means one per hardware thread). Each has its own listening socket, opened with `SO_REUSEPORT`, so the kernel distributes incoming connections among them, and each has its own copy of the router: workers share nothing on the request path. A connection stays on the worker that accepted it.

```cpp
#include <utility>

Router router;
router.setPath(http::Method::GET, "/hello", [](const HttpRequestView& req) { return req.makeResponse("hello\n"); });

HttpServer server(HttpServerConfig{}.withPort(8080).withNbThreads(4), std::move(router));
server.run();  // blocks until stop(), a drain, or a signal
```

## Creating a server

The constructor binds the listening socket: a server that was constructed is listening, and a bind failure throws. When `port` is `0`, the kernel chooses a free port, readable right away with `server.port()`, which is convenient for tests.

```cpp
SingleHttpServer server(HttpServerConfig{});  // port 0: ephemeral
const auto port = server.port();
```

A server can also be built from a JSON or YAML configuration file, see [Configuration files](../modules/json-and-config-files.md#configuration-files).

## Running

| Method | Blocks | Behavior |
| --- | --- | --- |
| `run()` | Yes | Runs the event loop(s) on the calling thread (and the worker threads), until `stop()`, the end of a drain, or a signal. |
| `runUntil(predicate)` | Yes | Same, also stopping when `predicate()` returns `true`, checked at each loop iteration. |
| `start()` | No | Runs in background threads owned by the server. |
| `startDetached()` | No | Same, returning an `AsyncHandle` that stops the server when destroyed. |
| `startDetachedAndStopWhen(predicate)`, `startDetachedWithStopToken(token)` | No | Background run stopped by a predicate, or a `std::stop_token`. |

```cpp
#include <chrono>
#include <thread>

SingleHttpServer server(HttpServerConfig{});
auto handle = server.startDetached();
// ... the calling thread is free ...
std::this_thread::sleep_for(std::chrono::seconds{1});
handle.stop();
handle.rethrowIfError();  // rethrows an exception that ended the event loop, if any
```

## Stopping

`stop()` and `beginDrain()` end a server in two different ways:

| | `stop()` | `beginDrain(maxWait)` |
| --- | --- | --- |
| New connections | Refused: the listening socket is closed. | Refused: the listening socket is closed. |
| Open connections | Closed right away. | Finish their current request; their response carries `Connection: close`. |
| End | Immediate. | When the last connection closed, or at `maxWait` (when not zero), where the remaining connections are closed. |
| Use it for | Fatal errors, tests. | Graceful shutdown and rolling updates. |

Both return immediately: the server stops asynchronously. `isDraining()` reports a drain in progress, and `isRunning()` stays `true` until the event loop has returned. Calling `beginDrain()` again with a shorter `maxWait` brings the deadline forward. While a server drains, its [readiness probe](../operations/health-probes.md) fails, so that load balancers stop sending it traffic.

### Signals

`SignalHandler::Enable(maxDrain)` installs `SIGINT` and `SIGTERM` handlers for the whole process: on a signal, every running server starts `beginDrain(maxDrain)` at its next loop iteration. This is what container orchestrators expect: Kubernetes sends `SIGTERM` and waits `terminationGracePeriodSeconds` before killing the process, so set `maxDrain` below that period.

```cpp
#include <chrono>

SignalHandler::Enable(std::chrono::seconds{20});

HttpServer server(HttpServerConfig{}.withPort(8080));
server.run();  // returns once the drain triggered by SIGTERM completed
```

The handler only sets a flag, read by the event loops. Applications that manage signals themselves can skip it and call `beginDrain()` directly.

## Restarting, copying, and moving

A server can be started again after `stop()` or a completed drain: `run()` rebuilds the listening sockets and event loops. An ephemeral port is kept across restarts; on Linux with `reusePort`, it stays reserved while the server is stopped, so that no other process can take it. Statistics restart from zero.

Servers can be copied and moved only while they are not running: moving or copying a running server throws `std::logic_error`. A copy shares nothing with its source, and must bind its own port, so stop or destroy the source first, or use `reusePort`.

## Updating a running server

`postConfigUpdate(updater)` changes the configuration of a running server, from any thread. The updater receives the configuration on the event-loop thread, at the next iteration:

```cpp
#include <chrono>

SingleHttpServer server(HttpServerConfig{});
server.start();
server.postConfigUpdate([](HttpServerConfig& config) {
  config.withMaxBodyBytes(1U << 20U).withKeepAliveTimeout(std::chrono::seconds{30});
});
```

- Most fields can change: limits, timeouts, compression, global headers, access log, TLS (see [hot reload](../protocols/tls.md#hot-reload-and-failure-behavior)).
- `port`, `reusePort`, `nbThreads`, and `telemetry` cannot: a change is logged and reverted.
- Each update is applied atomically: if the updater throws, if the result fails validation, or if a component cannot be rebuilt from it (an invalid certificate, an access log file that cannot be opened), the error is logged and the previous configuration stays in place.
- A `MultiHttpServer` applies the update to every worker.

Routes are updated the same way, with `router()` or `postRouterUpdate()`, see [Updating routes at runtime](routing.md#updating-routes-at-runtime).

## Connections and keep-alive

HTTP/1.1 connections are persistent by default: a client sends several requests on one connection, one after the other. `Connection` is a list of options: `close` closes the connection after the response wherever it appears, and an HTTP/1.0 connection persists only if it lists `keep-alive`.

| Setting | Default | Effect |
| --- | --- | --- |
| `enableKeepAlive` (`withKeepAliveMode()`) | `true` | `false` closes every connection after its response. |
| `keepAliveTimeout` | 5 s | An idle connection is closed after this duration. Also applies to WebSocket connections, see [Closing connections](../protocols/websocket.md#closing-connections). |
| `maxRequestsPerConnection` | 100,000 | The response to the last allowed request carries `Connection: close`. |
| `maxAcceptBatchSize` | 64 | New connections accepted per loop iteration, so that a connection burst does not starve the existing ones; `0` is unlimited. |

The server closes a connection right away, without waiting for the next request, after a protocol error (malformed request, conflicting framing headers, a limit exceeded): the parser state cannot be trusted anymore, so it never reads further requests from that connection.

## Port reuse

| `reusePort` | `SingleHttpServer` | `MultiHttpServer` with several threads |
| --- | --- | --- |
| `false` (default) | Exclusive bind: fails if the port is taken. | Checks the port is free with an exclusive bind, then binds its own workers with `SO_REUSEPORT`. |
| `true` | Binds with `SO_REUSEPORT` (`SO_REUSEADDR` on Windows): other sockets with the option may share the port. | Same, without the check. |

Keep the default unless several processes must share a port. With `SO_REUSEPORT`, any process of the same user binding the port with the option receives a share of the incoming connections.

## Tests

- Lifecycle, restart, copy, drain, signals, and configuration updates: [tests/http-server-lifecycle_test.cpp](../../tests/http-server-lifecycle_test.cpp).
- `MultiHttpServer`: [tests/multi-http-server_test.cpp](../../tests/multi-http-server_test.cpp).
- Keep-alive and `Connection` semantics: [tests/http-core_test.cpp](../../tests/http-core_test.cpp).
