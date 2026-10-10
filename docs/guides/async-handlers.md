# Async handlers

An async handler is a C++20 coroutine returning `RequestTask<HttpResponse>`. It can suspend while it waits for the request body or for blocking work running on a background thread, and the event loop serves the other connections meanwhile. This guide explains when async handlers pay off, how they behave, which rules keep them safe, and what they cost.

!!! tip "In one sentence"
    Use a synchronous handler for anything fast and in memory, and an async handler with `co_await req.deferWork(...)` for anything that blocks: database calls, calls to other services, file system or CPU-heavy work. Never block inside the coroutine body itself.

## Why the event loop must never block

An aeronet server thread runs one event loop that serves all its connections. A synchronous handler runs on that thread: while it runs, no other request of the same thread progresses, whatever its connection.

That is the right model for short handlers, which is most of them: no thread switch, no allocation, no synchronization. It breaks down as soon as a handler waits: one millisecond of blocking per request caps the event loop at roughly 1,000 requests per second, and every other client waits in line (see [Performance](#performance)).

An async handler splits the request in two:

- the coroutine body, which still runs on the event loop thread, and must stay short;
- `deferWork()` calls, which run the blocking parts on a background thread and resume the coroutine on the event loop once they complete.

## Choosing a handler type

| Workload | Handler | Why |
| --- | --- | --- |
| In-memory logic, cache hits, small computations (microseconds) | Synchronous | Cheapest path: no coroutine frame, no thread. |
| Large or slow uploads, early validation of the request head | Async (`bodyAwaitable()`, `readBodyAsync()`) | The handler is dispatched as soon as the head arrives. |
| Blocking I/O: database, other HTTP services, blocking SDKs, file system | Async + `deferWork()` | The event loop keeps serving other connections. |
| CPU-heavy work (hashing, image processing, compression) | Async + `deferWork()` if the machine has spare cores, otherwise synchronous on more server threads | A background thread only helps when it can run in parallel. |
| Large generated responses | Streaming (`HttpResponseWriter`) | Sends the response piece by piece; see [Streaming responses](streaming-responses.md). |
| Many clients waiting for server-side events | WebSocket | One `deferWork()` thread per waiting client does not scale to thousands; see [WebSocket](../protocols/websocket.md). |

## Your first async handler

```cpp
#include <aeronet/aeronet-server.hpp>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace aeronet;

namespace {

// Stands for any blocking call: a database client, a blocking SDK, a file read...
std::string LoadGreeting(std::string_view name) {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return "Hello, " + std::string(name) + "!\n";
}

}  // namespace

int main() {
  Router router;
  router.setPath(http::Method::GET, "/greet/{name}", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    // Runs on a background thread: the event loop serves the other connections meanwhile.
    std::string greeting =
        co_await req.deferWork([&req] { return LoadGreeting(req.pathParamValueOrEmpty("name")); });
    // Back on the event loop thread.
    co_return HttpResponse(200).body(std::move(greeting));
  });

  SingleHttpServer server(HttpServerConfig{}.withPort(8080), std::move(router));
  server.run();
}
```

Three things make this a coroutine handler:

- it takes a non-const `HttpRequestView&` (the request is not copied, and it stays valid until the coroutine completes);
- it returns `RequestTask<HttpResponse>`;
- it uses `co_await` / `co_return`.

`Router::setPath()` recognizes the signature, nothing else is needed. The work function captures the request by reference: this is safe, see [Lifetime and thread-safety rules](#lifetime-and-thread-safety-rules).

A runnable program with several endpoints is in [examples/async-handlers.cpp](../../examples/async-handlers.cpp) (binary `aeronet-async-handlers`).

## Registering async handlers

Every registration API accepts coroutine handlers: paths, route groups and the default handler. Route options (CORS, middleware, limits, timeouts) apply to them like to the other handler kinds.

```cpp
#include <string>
#include <string_view>

Router router;

// Several methods for the same handler.
router.setPath(http::Method::GET | http::Method::HEAD, "/status",
               [](HttpRequestView&) -> RequestTask<HttpResponse> { co_return HttpResponse(200).body("up\n"); });

// Route groups.
auto api = router.group("/api/v1");
api.setPath(http::Method::POST, "/jobs", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
  const std::string_view body = co_await req.bodyAwaitable();
  co_return HttpResponse(http::StatusCodeAccepted).body("queued " + std::to_string(body.size()) + " bytes\n");
});

// Fallback handler.
router.setDefault(
    [](HttpRequestView& req) -> RequestTask<HttpResponse> { co_return req.makeResponse(http::StatusCodeNotFound); });
```

A handler may also return immediately with `co_return`, without awaiting anything: it then completes during its dispatch, like a synchronous handler (at a slightly higher cost, see [Performance](#performance)).

An exception escaping the coroutine (from its body, or rethrown by `co_await`) produces a `500 Internal Server Error`, exactly like a throwing synchronous handler.

## How a request flows

### HTTP/1.1

1. The request head is parsed. The handler is dispatched **as soon as the head is complete**, even if the body is still being received: the coroutine can validate the head, start work, or decide to reject the request before the body has arrived.
2. Request middleware, CORS checks and `OPTIONS` / `TRACE` handling run first, like for any route. When one of them answers before the body was received, the server closes the connection after the response: the rest of the body cannot be skipped reliably.
3. The coroutine runs on the event loop until its first suspension:
    - `co_await req.bodyAwaitable()` / `readBodyAsync()` suspends until the whole body is received (it completes right away if it already is);
    - `co_await req.deferWork(work)` suspends until `work` returns on its background thread.
4. On `co_return`, the response middleware runs, then the response is sent. If the handler completed without reading the body, the response is sent once the body was fully received, to keep the connection in sync.
5. The requests pipelined behind it on the same connection are then served, in order.

While a handler runs with its whole request received, the server does not read the next bytes of the connection: they stay in the socket until the handler completes. This keeps the request memory stable (its views stay valid), bounds what a client can make the server buffer, and lets TCP flow control push back on a client sending more than it should.

### HTTP/2

1. Each stream is independent: a suspended handler never delays the other streams of the connection.
2. The handler is dispatched **once the whole request body is received** (`END_STREAM`): `bodyAwaitable()` and `readBodyAsync()` complete right away, and the per-route body limit is checked before the dispatch.
3. A stream reset by the client (`RST_STREAM`) or by a route timeout does not interrupt the running work: the coroutine is destroyed once its work completes, without being resumed.

### Summary

| Behavior | HTTP/1.1 | HTTP/2 |
| --- | --- | --- |
| Dispatch | As soon as the head is parsed | Once the whole body is received |
| Concurrency on one connection | One request at a time (pipelined requests are served in order) | One handler per stream, all in parallel |
| Client disconnects while work runs | Noticed after the handler completes (or by the route timeout) | `RST_STREAM`: the stream is closed, the work completes in the background |
| Route timeout expires | `408 Request Timeout`, then the connection is closed | `408 Request Timeout`, then `RST_STREAM` |
| Keep-alive idle timeout | Does not apply while the handler runs | Does not apply to a connection with active streams |

## Awaitables

### Reading the request body

```cpp
#include <cstdint>
#include <string>
#include <string_view>

Router router;

// Aggregated: the whole body at once.
router.setPath(http::Method::POST, "/upload", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
  const std::string_view body = co_await req.bodyAwaitable();
  co_return HttpResponse(200).body("received " + std::to_string(body.size()) + " bytes\n");
});

// In chunks, for instance to feed an incremental parser or a hash.
router.setPath(http::Method::POST, "/checksum", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
  std::uint32_t checksum = 0;
  while (req.hasMoreBody()) {
    const std::string_view chunk = co_await req.readBodyAsync(16UL * 1024UL);
    for (const char ch : chunk) {
      checksum = (checksum * 31U) + static_cast<unsigned char>(ch);
    }
  }
  co_return HttpResponse(200).body(std::to_string(checksum) + "\n");
});
```

- The body is buffered by the server (decompressed if needed, de-chunked), within the `maxBodyBytes` limits of the server and of the route. `readBodyAsync()` gives a chunked access to it; it does not stream the upload from the socket.
- A request is read either with `bodyAwaitable()` / `body()`, or with `readBodyAsync()` / `readBody()`: once one style was used, the other one throws `std::logic_error`.
- Trailers (chunked requests, HTTP/2) are available through `req.trailers()` once the body was awaited.
- `Expect: 100-continue` is answered by the server when the handler is dispatched.

### Running blocking work with deferWork()

`req.deferWork(work)` starts `work` on a new background thread, suspends the coroutine, and resumes it on the event loop thread with the value returned by `work`.

```cpp
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

struct Report {
  explicit Report(int lines) : lineCount(lines) {}

  int lineCount;
};

Router router;
router.setPath(http::Method::POST, "/reports", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
  // A work function may return nothing...
  co_await req.deferWork([] { /* warm a cache, write an audit record... */ });

  // ... be move-only...
  auto settings = std::make_unique<std::string>("compact");
  const std::size_t settingsSize = co_await req.deferWork([owned = std::move(settings)] { return owned->size(); });

  // ... and return a type that is not default constructible.
  const Report report = co_await req.deferWork([] { return Report(42); });

  // An exception thrown by the work function is rethrown by co_await.
  std::string error;
  try {
    co_await req.deferWork([] { throw std::runtime_error("database unavailable"); });
  } catch (const std::exception& ex) {
    error = ex.what();
  }
  if (!error.empty()) {
    co_return HttpResponse(http::StatusCodeServiceUnavailable).body(error);
  }

  co_return HttpResponse(200).body(std::to_string(report.lineCount + static_cast<int>(settingsSize)));
});
```

Key properties:

- **Return value**: `co_await` returns the value returned by the work function (a returned reference is copied, the work being over when the coroutine resumes).
- **Exceptions**: thrown by the work function, they are rethrown by `co_await`. If no thread can be started (thread limit reached), the `std::system_error` is rethrown by `co_await` as well.
- **One thread per call**: each call creates (and detaches) an OS thread. Group consecutive blocking calls in one `deferWork()` when they do not need the event loop in between.
- **Resumption**: the coroutine always resumes on the event loop thread of its connection. Everything between two `co_await` runs there, like a synchronous handler.
- **No cancellation**: a running work function cannot be interrupted. Bound its duration in the blocking call itself (database statement timeouts, client timeouts...).

### Supported awaitables

A `RequestTask` coroutine accepts exactly these awaitables (the `RequestTaskAwaitable` concept): `bodyAwaitable()`, `readBodyAsync()`, `deferWork()`, `std::suspend_always` and `std::suspend_never`. Any other awaitable fails to compile.

The reason: the server knows when its own awaitables complete. A coroutine suspended by an awaitable it does not know is resumed right away, and an awaitable resuming the coroutine by itself later (from another thread, typically) would resume it twice. To integrate an asynchronous library, call its blocking API (or wait for its future) inside `deferWork()`.

An awaitable that never suspends, or that resumes the coroutine before returning from `await_suspend()`, can opt in with an `AeronetAwaitableTag` member:

```cpp
#include <coroutine>

// Completes synchronously (await_ready() is always true): safe to await in a RequestTask.
struct CachedValue {
  using AeronetAwaitableTag = void;

  [[nodiscard]] bool await_ready() const noexcept { return true; }
  void await_suspend(std::coroutine_handle<> /*handle*/) const noexcept {}
  [[nodiscard]] int await_resume() const noexcept { return value; }

  int value;
};

static_assert(RequestTaskAwaitable<CachedValue>);

Router router;
router.setPath(http::Method::GET, "/cached", [](HttpRequestView&) -> RequestTask<HttpResponse> {
  const int value = co_await CachedValue{42};
  co_return HttpResponse(200).body(std::to_string(value));
});
```

A `RequestTask` is not awaitable itself: share code between handlers with regular functions, called from the coroutine or from a work function.

## Lifetime and thread-safety rules

### What stays valid

While a handler is running, including across suspensions:

- **The request** (method, path, headers, query and path parameters, body, trailers): all its views stay valid until the coroutine completes. The server copies the request head out of the connection buffer when the handler is dispatched, and does not touch the input buffer while the handler holds it.
- **The coroutine frame** (its local variables) and **the request** stay alive until the running work completes, **even if the connection is closed or the HTTP/2 stream reset meanwhile**. The coroutine is then destroyed without being resumed. A work function may therefore capture locals and the request by reference.
- **The handler object** (the lambda and its captures) stays alive until the coroutine completes, even if the router replaces or removes the route meanwhile.
- **The server**: destroying a server waits for the work functions still running for it.

### What a work function may do

A work function runs on its own thread, concurrently with the event loop and with other work functions.

| In a work function | Safe? |
| --- | --- |
| Read the request head: `path()`, `method()`, headers, query and path parameters | Yes: the server does not modify them while the coroutine is suspended |
| Read the body obtained with `co_await req.bodyAwaitable()` **before** calling `deferWork()` | Yes |
| Read or write local variables of the coroutine (captured by reference) | Yes: the coroutine is suspended until the work completes |
| Call `req.body()`, `readBody()`, or await anything on the request | No: await the body in the coroutine first, then pass it to the work |
| Build the `HttpResponse` | Prefer the coroutine, after `co_await` |
| Use application state shared with other requests | Only with synchronization (mutex, atomics, thread-safe pool) |
| Use the router, the server, or `HttpServerConfig` | No |
| Keep `string_view`s of the request after the handler completes | No: copy what must outlive the request |

### Application state

Work functions of different requests run in parallel: any state they share must be thread-safe. Objects used by work functions must also outlive the server, since the server waits for running work in its destructor: declare them before the server.

```cpp
#include <charconv>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

using namespace aeronet;

namespace {

// Stands for a blocking database client. Used by concurrent work functions: it must be thread-safe (here a mutex,
// typically a connection pool).
class UserRepository {
 public:
  std::optional<std::string> findName(int id) {
    std::scoped_lock lock(_mutex);
    const auto it = _names.find(id);
    if (it == _names.end()) {
      return std::nullopt;
    }
    return it->second;
  }

 private:
  std::mutex _mutex;
  std::unordered_map<int, std::string> _names{{1, "Alice"}, {2, "Bob"}};
};

int ParseId(std::string_view text) {
  int id = 0;
  std::from_chars(text.data(), text.data() + text.size(), id);
  return id;
}

}  // namespace

int main() {
  // Declared before the server: destroyed after it, once the running work completed.
  UserRepository repository;

  Router router;
  router.setPath(http::Method::GET, "/users/{id}", [&repository](HttpRequestView& req) -> RequestTask<HttpResponse> {
    const int id = ParseId(req.pathParamValueOrEmpty("id"));
    const std::optional<std::string> name = co_await req.deferWork([&repository, id] { return repository.findName(id); });
    if (!name) {
      co_return req.makeResponse(http::StatusCodeNotFound);
    }
    co_return req.makeResponse("{\"name\":\"" + *name + "\"}", http::ContentTypeApplicationJson);
  });

  SingleHttpServer server(HttpServerConfig{}.withPort(8080), std::move(router));
  server.run();
}
```

!!! warning "Background threads are short-lived"
    Each `deferWork()` call runs on a new thread, so `thread_local` caches do not survive from one call to the next. Keep reusable resources (database connections, HTTP clients) in a thread-safe pool owned by the application.

## Use cases

### Process an upload off the event loop

The body stays valid while the coroutine is suspended: the work function can read it in place, without a copy.

```cpp
#include <string>
#include <string_view>

Router router;
router.setPath(http::Method::POST, "/thumbnails", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
  const std::string_view image = co_await req.bodyAwaitable();
  std::string thumbnail = co_await req.deferWork([image] {
    // CPU-heavy processing of the uploaded bytes (decode, resize, encode...).
    return std::string(image.substr(0, image.size() / 4));
  });
  co_return req.makeResponse(thumbnail, "image/png");
});
```

### Reject a request before its upload

An async route is dispatched as soon as the request head arrives. Request middleware runs before the handler: a middleware answering before the body was received makes the server respond right away and close the connection, so a client is not kept uploading a body that will be refused.

```cpp
#include <string>
#include <string_view>

Router router;
auto& upload = router.setPath(http::Method::PUT, "/files/{name}", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
  const std::string_view content = co_await req.bodyAwaitable();
  co_return HttpResponse(http::StatusCodeCreated).body("stored " + std::to_string(content.size()) + " bytes\n");
});
upload.before([](HttpRequestView& req) {
  if (req.headerValueOrEmpty("authorization") != "Bearer secret-token") {
    return MiddlewareResult::ShortCircuit(HttpResponse(http::StatusCodeUnauthorized, "missing credentials\n"));
  }
  return MiddlewareResult::Continue();
});
upload.maxBodyBytes(64UL << 20U);
```

The same applies to CORS rejections. A coroutine that returns its response without awaiting the body is different: its response is only sent once the body was received.

### Call another HTTP service

`HttpClient` is blocking: call it from a work function. An `HttpClient` instance is not thread-safe, so do not share one between concurrent work functions without a lock.

```cpp
#include <aeronet/http-client.hpp>
#include <string>
#include <utility>

Router router;
router.setPath(http::Method::GET, "/weather/{city}", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
  std::string url = "http://weather.internal/api/" + std::string(req.pathParamValueOrEmpty("city"));
  auto upstream = co_await req.deferWork([url = std::move(url)] {
    HttpClient client;
    return client.get(url);
  });
  if (!upstream) {
    co_return req.makeResponse(http::StatusCodeBadGateway, "weather service unreachable\n");
  }
  co_return req.makeResponse(upstream->bodyInMemory(), http::ContentTypeApplicationJson);
});
```

### Chain dependent calls, group independent ones

Each `deferWork()` costs a thread creation and a round trip through the event loop. Chain calls when a step needs the event loop (to inspect a result, or to decide whether to continue); otherwise run them in a single work function.

```cpp
#include <future>
#include <string>

Router router;

// Dependent steps: the second call needs the first result.
router.setPath(http::Method::GET, "/orders/{id}/invoice", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
  const std::string customer = co_await req.deferWork([&req] {
    return "customer-of-" + std::string(req.pathParamValueOrEmpty("id"));  // first query
  });
  if (customer.empty()) {
    co_return req.makeResponse(http::StatusCodeNotFound);
  }
  std::string invoice = co_await req.deferWork([&customer] { return "invoice for " + customer; });  // second query
  co_return req.makeResponse(invoice);
});

// Independent calls: one background thread, and std::async to run them in parallel.
router.setPath(http::Method::GET, "/dashboard", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
  std::string page = co_await req.deferWork([] {
    auto orders = std::async(std::launch::async, [] { return std::string("orders"); });
    auto invoices = std::async(std::launch::async, [] { return std::string("invoices"); });
    return orders.get() + "," + invoices.get();
  });
  co_return req.makeResponse(page);
});
```

### Bound the number of running work functions

Every running `deferWork()` holds an OS thread, a coroutine frame and its connection. Under a traffic burst, or when a backend slows down, their number can grow quickly; past the system thread limit, `co_await` throws `std::system_error`. Bound them explicitly and shed the excess load with `503 Service Unavailable`:

```cpp
#include <atomic>
#include <string>
#include <system_error>
#include <utility>

using namespace aeronet;

namespace {

// Holds a slot of a bounded counter for its lifetime. The coroutine frame owning it is destroyed when its work is over,
// even if the connection was closed meanwhile: the counter tracks the threads actually running.
class Slot {
 public:
  Slot(std::atomic<int>& counter, int limit) : _counter(counter) {
    _acquired = _counter.fetch_add(1, std::memory_order_relaxed) < limit;
  }

  Slot(const Slot&) = delete;
  Slot& operator=(const Slot&) = delete;

  ~Slot() { _counter.fetch_sub(1, std::memory_order_relaxed); }

  [[nodiscard]] bool acquired() const noexcept { return _acquired; }

 private:
  std::atomic<int>& _counter;
  bool _acquired;
};

HttpResponse Busy() {
  HttpResponse response(http::StatusCodeServiceUnavailable, "busy, retry later\n");
  response.header(http::RetryAfter, "1");
  return response;
}

}  // namespace

int main() {
  // Shared by all the server threads.
  std::atomic<int> running{0};
  static constexpr int kMaxRunning = 512;

  Router router;
  router.setPath(http::Method::GET, "/reports/{id}", [&running](HttpRequestView& req) -> RequestTask<HttpResponse> {
    const Slot slot(running, kMaxRunning);
    if (!slot.acquired()) {
      co_return Busy();
    }
    std::string report;
    bool started = true;
    try {
      report = co_await req.deferWork([&req] { return "report " + std::string(req.pathParamValueOrEmpty("id")); });
    } catch (const std::system_error&) {
      started = false;  // no thread available
    }
    if (!started) {
      co_return Busy();
    }
    co_return req.makeResponse(report);
  });

  HttpServer server(HttpServerConfig{}.withPort(8080).withNbThreads(4), std::move(router));
  server.run();
}
```

### Time out slow requests

A per-route timeout answers `408 Request Timeout` and closes the connection (HTTP/1.1) or resets the stream (HTTP/2) when the handler takes too long. The work function keeps running until it returns (its result is then discarded): pair the route timeout with a timeout in the blocking call itself, so that threads do not pile up behind a stuck backend.

```cpp
#include <chrono>
#include <string>
#include <thread>

Router router;
auto& slowReport = router.setPath(http::Method::GET, "/slow-report", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
  std::string report = co_await req.deferWork([] {
    std::this_thread::sleep_for(std::chrono::seconds(1));  // e.g. a query with a 2 s statement timeout
    return std::string("report\n");
  });
  co_return req.makeResponse(report);
});
slowReport.timeout(std::chrono::seconds(3));

// The same timeout for a whole group of routes.
auto reports = router.group("/reports");
reports.withTimeout(std::chrono::seconds(3));
reports.setPath(http::Method::GET, "/daily",
                [](HttpRequestView& req) -> RequestTask<HttpResponse> { co_return req.makeResponse("daily\n"); });
```

There is no timeout by default: without one, a work function that never returns holds its connection (and its thread) forever, and blocks the destruction of the server.

### Wait for an event (long polling)

A work function can wait for an application event, with a deadline. Each waiting client holds a thread: this suits a moderate number of clients; prefer [WebSocket](../protocols/websocket.md) to push events to many of them.

```cpp
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

using namespace aeronet;

namespace {

struct EventBus {
  std::mutex mutex;
  std::condition_variable changed;
  std::uint64_t version{0};
  std::string lastEvent;
};

}  // namespace

int main() {
  // Declared before the server: work functions may still wait on it until the server is destroyed.
  EventBus bus;

  Router router;
  router
      .setPath(http::Method::GET, "/events/next",
               [&bus](HttpRequestView& req) -> RequestTask<HttpResponse> {
                 const auto since = req.queryParamInt<std::uint64_t>("since").value_or(0);
                 std::optional<std::string> event = co_await req.deferWork([&bus, since] -> std::optional<std::string> {
                   std::unique_lock lock(bus.mutex);
                   if (!bus.changed.wait_for(lock, std::chrono::seconds(25), [&] { return bus.version > since; })) {
                     return std::nullopt;
                   }
                   return bus.lastEvent;
                 });
                 if (!event) {
                   co_return req.makeResponse(http::StatusCodeNoContent);
                 }
                 co_return req.makeResponse(*event);
               })
      .timeout(std::chrono::seconds(30));

  SingleHttpServer server(HttpServerConfig{}.withPort(8080), std::move(router));
  server.run();
}
```

## Router updates, drain and shutdown

- **Router updates** (`server.router()`, `postRouterUpdate()`) can be applied while handlers are suspended. A suspended handler keeps its own handler object alive. The response middleware and the CORS policy applied to its response are the ones of the route matching its request when it completes (a removed route no longer contributes any).
- **Graceful drain** (`beginDrain()`): in-flight async requests complete normally (their responses close the connection) within the drain deadline; connections still open at the deadline are closed.
- **Stop**: `stop()` closes the connections; their running work functions complete in the background, without resuming their coroutines.
- **Destruction**: the destructor of a server waits for the work functions still running (it logs a warning every 5 seconds while waiting). Keep work functions bounded in time.

## Performance

### Cost model

| Step | Cost |
| --- | --- |
| Dispatching an async handler | One allocation for the coroutine frame (its size depends on the local variables), a copy of the request head, and a reference count increment on the handler |
| `co_await` of the body, already received | Nothing: it completes without suspending |
| `co_await req.deferWork(work)` | One OS thread created (by the event loop thread) and detached, a few small allocations (result state, thread start), one wake-up of the event loop, one resumption |
| Waiting | Nothing on the event loop: a suspended handler costs memory only (its frame, its connection buffers, and the stack of its background thread) |

### Measurements

Indicative measurements of a single event loop thread (one `SingleHttpServer`, release build), on a 12-core AMD Ryzen AI 9 HX PRO 370 under Linux. Load generated by `wrk -t4 -c64` over loopback, on 4 other cores. Each handler answers a 2-byte body.

| Endpoint | Handler | Server CPUs | Requests/s | Latency p50 | Latency p99 |
| --- | --- | --- | --- | --- | --- |
| Trivial | Synchronous | 1 | 190,000 | 0.33 ms | 0.38 ms |
| Trivial | Async, no suspension | 1 | 178,000 | 0.36 ms | 0.45 ms |
| Trivial | Async + `deferWork()` | 1 | 53,000 | 1.07 ms | 2.4 ms |
| Trivial | Async + `deferWork()` | 8 | 70,000 | 0.88 ms | 1.5 ms |
| 1 ms blocking call | Synchronous | 1 | 900 | 68 ms | 75 ms |
| 1 ms blocking call | Async + `deferWork()` | 1 | 22,000 | 2.6 ms | 5 to 13 ms |
| 1 ms blocking call | Async + `deferWork()` | 8 | 53,000 | 1.2 ms | 1.6 ms |
| 50 µs of CPU work | Synchronous | 1 | 17,600 | 3.6 ms | 4.2 ms |
| 50 µs of CPU work | Async + `deferWork()` | 1 | 12,200 | 5.2 ms | 9.4 ms |
| 50 µs of CPU work | Async + `deferWork()` | 8 | 60,000 | 0.97 ms | 1.9 ms |

"Server CPUs" is the set of cores the server process (its event loop and the threads of `deferWork()`) may run on. Absolute numbers depend on the machine, the network and the handlers; the ratios between the rows are what matters.

### What the numbers mean

- **A coroutine without suspension costs a few percent** (6% here, on a handler that does nothing): the frame allocation and the head copy. For handlers doing real work, the difference vanishes; still, do not turn fast synchronous handlers into coroutines without a reason.
- **`deferWork()` costs about 10 to 15 µs of event loop time per call** in this setup, mostly the thread creation. It pays off for blocking calls lasting from a few hundred microseconds; below that, a synchronous handler is cheaper.
- **Blocking the event loop is the worst case**: with 1 ms of blocking per request, the synchronous handler serves 25 to 60 times fewer requests, and every client waits behind the others (68 ms median latency instead of 1 to 3 ms).
- **CPU-heavy work only benefits from `deferWork()` with spare cores**: on a single core, it adds thread overhead and is slower than a synchronous handler. Without spare cores, run more server threads (`HttpServerConfig::withNbThreads()`) instead.

### Guidelines

- Keep the coroutine body short: everything outside `deferWork()` runs on the event loop thread, blocking calls included.
- Group consecutive blocking calls in one work function; chain `deferWork()` calls only when a step needs the event loop.
- Bound the number of running work functions (see [Bound the number of running work functions](#bound-the-number-of-running-work-functions)): each one holds a thread (whose stack reserves 8 MiB of virtual memory on Linux by default, a few pages being actually used), a frame and a connection.
- Set route timeouts, and timeouts in the blocking calls themselves.
- For parallelism within one client, prefer HTTP/2 (independent streams) or several connections: an HTTP/1.1 connection serves its requests one at a time.
- Size the server threads (`withNbThreads()`) for the work done on the event loops, and the machine for the work done in the background threads.

## Common pitfalls

- **Blocking in the coroutine body**: only the work function runs on a background thread. A blocking call written directly in the coroutine blocks the event loop like a synchronous handler.
- **Reading the body from a work function before awaiting it**: on HTTP/1.1 the handler may start before the body is received. Await it in the coroutine, then pass it to the work function.
- **Unsynchronized shared state**: concurrent work functions run on different threads.
- **Unbounded concurrency**: one thread per running `deferWork()`. A slow backend can exhaust the threads of the process.
- **Expecting cancellation**: a closed connection, a reset stream or a timeout does not stop a running work function.
- **Foreign awaitables**: only aeronet's awaitables (and `std::suspend_always` / `std::suspend_never`) compile in a `RequestTask`; wrap other asynchronous APIs in `deferWork()`.

## Limitations

- Each `deferWork()` call creates a thread: there is no built-in thread pool yet.
- Running work cannot be cancelled.
- Request bodies are buffered (within `maxBodyBytes`) before `bodyAwaitable()` / `readBodyAsync()` complete: there is no incremental upload streaming.
- Coroutines cannot await other coroutines (`RequestTask` is not awaitable).

## Reference

- Request views and their lifetime: [Memory management and string_view safety](requests.md#lifetime-of-request-data).
- Route options (timeouts, limits): [Routing and requests](routing.md) and the [server configuration reference](../reference/server-configuration.md).
- Runnable example: [examples/async-handlers.cpp](../../examples/async-handlers.cpp).
