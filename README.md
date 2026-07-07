# aeronet

<p align="center">
  <img src="resources/logo-blue-violet.png" alt="aeronet" width="360" />
</p>

[![CI](https://github.com/sjanel/aeronet/actions/workflows/ci.yml/badge.svg)](https://github.com/sjanel/aeronet/actions/workflows/ci.yml)
[![Coverage](https://codecov.io/gh/sjanel/aeronet/branch/main/graph/badge.svg)](https://codecov.io/gh/sjanel/aeronet)
[![Packaging](https://github.com/sjanel/aeronet/actions/workflows/packaging.yml/badge.svg)](https://github.com/sjanel/aeronet/actions/workflows/packaging.yml)
[![H1 Benchmarks](https://img.shields.io/endpoint?url=https%3A%2F%2Fsjanel.github.io%2Faeronet%2Fbenchmarks%2Fbenchmark_badge.json)](https://sjanel.github.io/aeronet/benchmarks/)
[![H2 h2c Benchmarks](https://img.shields.io/endpoint?url=https%3A%2F%2Fsjanel.github.io%2Faeronet%2Fbenchmarks%2Fh2%2Fbenchmark_badge_h2c.json)](https://sjanel.github.io/aeronet/benchmarks/h2/benchmarks_h2c.html)
[![H2 TLS Benchmarks](https://img.shields.io/endpoint?url=https%3A%2F%2Fsjanel.github.io%2Faeronet%2Fbenchmarks%2Fh2%2Fbenchmark_badge.json)](https://sjanel.github.io/aeronet/benchmarks/h2/benchmarks_h2-tls.html)
[![WS Benchmarks](https://img.shields.io/endpoint?url=https%3A%2F%2Fsjanel.github.io%2Faeronet%2Fbenchmarks%2Fws%2Fws_benchmark_badge.json)](https://sjanel.github.io/aeronet/benchmarks/ws/)
[![H1 Client Benchmarks](https://img.shields.io/endpoint?url=https%3A%2F%2Fsjanel.github.io%2Faeronet%2Fbenchmarks%2Fclients%2Fclient_benchmark_badge.json)](https://sjanel.github.io/aeronet/benchmarks/clients/)
[![H2 h2c Client Benchmarks](https://img.shields.io/endpoint?url=https%3A%2F%2Fsjanel.github.io%2Faeronet%2Fbenchmarks%2Fclients%2Fclient_benchmark_badge_h2c.json)](https://sjanel.github.io/aeronet/benchmarks/clients/client_benchmark_h2c.html)
[![H2 TLS Client Benchmarks](https://img.shields.io/endpoint?url=https%3A%2F%2Fsjanel.github.io%2Faeronet%2Fbenchmarks%2Fclients%2Fclient_benchmark_badge_h2-tls.json)](https://sjanel.github.io/aeronet/benchmarks/clients/client_benchmark_h2-tls.html)
[![Release](https://img.shields.io/github/v/release/sjanel/aeronet?style=flat-square)](https://github.com/sjanel/aeronet/releases/latest)

## Why aeronet?

**aeronet** is a modern, fast, modular and ergonomic HTTP / WebSocket C++ **server & client** library for **Linux**, **macOS** and **Windows** focused on predictable performance, explicit control and minimal dependencies.

- **Fast & predictable**: edge‑triggered reactor model, zero/low‑allocation hot paths and minimal copies, horizontal scaling with port reuse. In CI benchmarks `aeronet` ranks among the [fastest tested implementations](#performance-at-a-glance) across multiple realistic scenarios.
- **Modular & opt‑in**: enable only the features you need at compile time to minimize binary size and dependencies
- **Ergonomic**: easy API, automatic features (encoding, telemetry), RAII listener setup with sync / async server lifetime control, developer friendly with no hidden global state, no macros
- **Configurable**: extensive dynamic configuration with reasonable defaults, per path options and middleware helpers, run-time router / config updates
- **Standards compliant**: HTTP/1.1, HTTP/2, WebSocket, Compression, Streaming, Trailers, TLS, CORS, Range & Conditional Requests, Static files, URL Decoding, multipart/form-data, etc.
- **Batteries included**: beyond the server, a matching HTTP **client** and a **JWT** (JWS) module for signing/verifying tokens — same opt‑in, zero‑extra‑dependency design (both reuse the server's own transport / TLS / crypto bricks).
- **Cloud native**: Built-in Kubernetes-style health probes, opentelemetry support (metrics, tracing) with built-in spans and metrics, dogstatsd support, perfect for micro-services
- **Cross‑platform**: primary platform is **Linux** (epoll); macOS (kqueue) and Windows (WSAPoll) are supported with a portable abstraction layer. Some Linux‑specific optimizations (kTLS, `MSG_ZEROCOPY`, `sendfile`) are automatically disabled on other platforms.

### Performance at a glance

`aeronet` is designed to be **very fast**, optimized for **Linux**. In our automated [wrk](https://github.com/wg/wrk)-based benchmarks (HTTP/1.1), [h2load](https://nghttp2.org/documentation/h2load-howto.html)-based benchmarks (HTTP/2), and [k6](https://k6.io/)-based benchmarks (WebSocket) against other popular frameworks (run in CI against a fixed set of competitors such as [drogon](https://github.com/drogonframework/drogon), [pistache](https://github.com/pistacheio/pistache), [Crow](https://github.com/CrowCpp/Crow), a [Rust Axum server](https://docs.rs/axum/latest/axum/), [Undertow in Java](https://github.com/undertow-io/undertow), Go and Python), `aeronet`:

- Achieves the **highest requests/sec** in most scenarios
- Consistently delivers **lower average latency** in those same scenarios
- Maintains **competitive or better throughput and memory usage**

You can inspect the latest benchmark tables generated on `main` from the CI **benchmarks** job and detailed methodology here:

- [Latest CI benchmarks (HTTP/1.1, HTTP/2, WebSocket)](https://github.com/sjanel/aeronet/actions/workflows/benchmarks-gh-pages.yml?query=branch%3Amain)
- [Benchmark scenarios and methodology](benchmarks/scripted-servers/README.md)

You can browse the latest rendered benchmark tables directly on GitHub Pages:

- [HTTP/1.1 Live benchmark dashboard](https://sjanel.github.io/aeronet/benchmarks/)
- [HTTP/2 clear-text Live benchmark dashboard](https://sjanel.github.io/aeronet/benchmarks/h2/benchmarks_h2c.html)
- [HTTP/2 TLS Live benchmark dashboard](https://sjanel.github.io/aeronet/benchmarks/h2/benchmarks_h2-tls.html)
- [WebSocket Live benchmark dashboard](https://sjanel.github.io/aeronet/benchmarks/ws/)
- [HTTP/1.1 client Live benchmark dashboard](https://sjanel.github.io/aeronet/benchmarks/clients/) (aeronet `HttpClient` vs libcurl / drogon / beast)
- [HTTP/2 clear-text client Live benchmark dashboard](https://sjanel.github.io/aeronet/benchmarks/clients/client_benchmark_h2c.html) (aeronet `HttpClient` vs libcurl, h2c)
- [HTTP/2 TLS client Live benchmark dashboard](https://sjanel.github.io/aeronet/benchmarks/clients/client_benchmark_h2-tls.html) (aeronet `HttpClient` vs libcurl, ALPN `h2`)

## Minimal Examples

Spin up a basic HTTP server that responds on `/hello` in just a few lines.
**All code examples** in the `README` and the `FEATURES.md` files are guaranteed to compile as they are covered by a CI check.

### Immediate response

Return a complete, immediate `HttpResponse` from the handler:

```cpp
#include <aeronet/aeronet.hpp> // unique 'umbrella' header, includes all public API

using namespace aeronet;

int main() {
  Router router;
  router.setPath(http::Method::GET, "/hello", [](const HttpRequest& req) {
    return HttpResponse(200).header("X-Req-Body", req.body()).body("hello from aeronet\n");
  });
  HttpServer server(HttpServerConfig{}, std::move(router)); // default port is ephemeral, OS will pick an available one
  server.run(); // blocking. Use start() for non-blocking
}
```

See the [full program](examples/server-minimal.cpp).

### Streaming response

When the response body size is not known upfront, or when you want to transmit the response as several controlled-size network chunks rather than a single transmission, use `HttpResponseWriter`. It automatically applies HTTP chunked transfer encoding, so each `writeBody()` call maps to its own network chunk - unlike a normal handler where all `bodyAppend()` calls are buffered and sent as a single transmission at the end:

```cpp
Router router;
router.setDefault([](const HttpRequest& req, HttpResponseWriter& writer){
  writer.status(200);
  writer.header("X-Req-Path", req.path());
  writer.contentType("text/plain");
  for (int i = 0; i < 10; ++i) {
    writer.writeBody(std::string(50,'x')); // each call is its own network chunk
  }
  writer.end();
});
```

### Async handler (Coroutines)

For a large request body or an asynchronous operation that may take a long time, use an async handler returning `RequestTask<HttpResponse>`:

```cpp
// Minimal awaitable used for the README demo so `co_await someAsyncOperation()` compiles.
struct SomeAsyncAwaitable {
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) noexcept { h.resume(); }
  std::string await_resume() const noexcept { return std::string("Hello from coroutine!"); }
};

SomeAsyncAwaitable someAsyncOperation() { return {}; }

int main() {
  Router router;
  router.setPath(http::Method::GET, "/async", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    // Suspend execution without blocking the thread
    auto result = co_await someAsyncOperation();
    co_return HttpResponse(200).body(result);
  });
}
```

Async handlers are invoked as soon as the request head is parsed, even if the body is still streaming in.
Call `co_await req.bodyAwaitable()` (or the chunked helpers) before touching the body to wait for the buffered payload.

You can refer to the [complete async handlers example](examples/async-handlers.cpp) for more details.

## Quick Start with provided examples

Minimal server examples for typical use cases are provided in [examples](examples) directory.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/examples/aeronet-server-minimal 8080   # or omit 8080 for ephemeral
```

Test with curl:

```bash
curl -i http://localhost:8080/hello

HTTP/1.1 200
date: Wed, 11 Feb 2026 22:46:04 GMT
content-type: text/plain
content-length: 151
server: aeronet

Hello from aeronet minimal server! You requested /hello
```

## Feature Overview

A bird's-eye view of what's implemented, what's still experimental, and where to read the full details.

### Feature Matrix (Concise)

| Category | Implemented (✔) | Notes |
|----------|-----------------|-------|
| Core HTTP/1.1 parsing | ✔ | Request line, headers, chunked bodies, pipelining |
| Routing | ✔ | Exact path + method allow‑lists; streaming + fixed |
| Keep‑Alive / Limits | ✔ | Header/body size, max requests per connection, idle timeout |
| Compression (gzip/deflate/zstd/br) | ✔ | Flags opt‑in; q‑value negotiation; threshold; per‑response opt‑out; direct compression |
| Inbound body decompression | ✔ | Multi‑layer, safety guards, header removal |
| TLS | ✔ (flag) | ALPN, mTLS, session tickets, kTLS sendfile, timeouts, metrics |
| OpenTelemetry | ✔ (flag) | Distributed tracing spans, metrics counters (experimental) |
| Async wrapper | ✔ | Background thread convenience |
| Metrics hook | ✔ (alpha) | Per‑request basic stats |
| Logging | ✔ (flag) | spdlog optional |
| Duplicate header policy | ✔ | Deterministic, security‑minded |
| WebSocket | ✔ | RFC 6455 compliant, text/binary frames, ping/pong, close handshake |
| HTTP/2 | ✔ (flag) | RFC 9113, HPACK, ALPN h2, h2c upgrade, stream multiplexing |
| Trailers exposure | ✔ | RFC 7230 §4.1.2 chunked trailer headers |
| Middleware helpers | ✔ | Global + per-route request/response hooks (streaming-aware) |
| Streaming inbound decompression | ✔ | Auto-switches to streaming inflaters once Content-Length exceeds configured threshold |
| sendfile / static file helper | ✔ | 0.4.x – zero-copy plain sockets plus RFC 7233 single-range & RFC 7232 validators |
| HTTP client | ✔ (flag) | Sync HTTP/1.1, keep‑alive pool, redirects, auto (de)compression, HTTPS |
| JWT (JWS) | ✔ (flag) | RFC 7519 sign/verify; HS/RS/ES/PS/EdDSA; JWK/JWKS by `kid`; rejects `alg:none` |

### Developer / Operational Features

| Feature | Notes |
|---------|-------|
| Epoll edge-triggered loop | One thread per `SingleHttpServer`; writev used for header+body scatter-gather |
| `SO_REUSEPORT` scaling | Horizontal multi-reactor capability |
| Multi-instance wrapper | `MultiHttpServer` orchestrates N reactors (N threads) |
| Async server methods | `start()` (void convenience) and `startDetached()` (returns `AsyncHandle`) |
| Move semantics | Transfer listening socket & loop state safely |
| Restarts | `SingleHttpServer` and `MultiHttpServer` can be started again after stop |
| Graceful draining | `SingleHttpServer::beginDrain(maxWait)` stops new accepts, closes keep-alive after current responses, optional deadline to force-close stragglers |
| Signal handling | Optional built-in SIGINT/SIGTERM handler to initiate draining when stop requested |
| Heterogeneous lookups | Path handler map accepts `std::string`, `std::string_view`, `const char*` |
| Outbound stats | Bytes queued, immediate vs flush writes, high-water marks |
| Lightweight logging | Pluggable design (spdlog optional); ISO 8601 UTC timestamps |
| Builder-style config | Fluent `HttpServerConfig` setters (`withPort()`, etc.) |
| Metrics callback | Per-request timing & size scaffold hook |
| RAII construction | Fully listening after constructor (ephemeral port resolved immediately) |
| Comprehensive tests | Parsing, limits, streaming, mixed precedence, reuseport, move semantics, keep-alive |
| Mixed handlers example | Normal + streaming coexistence on same path (e.g. GET streaming, POST fixed) |

### HTTP/1.1 Feature Matrix

For the exhaustive, continually-updated matrix (parsing, transport, bodies, status handling, headers, etc.) and architecture notes, see:

- [HTTP/1.1 Feature Matrix](docs/FEATURES.md#http11-feature-matrix)
- [Performance / architecture](docs/FEATURES.md#performance--architecture)

### Detailed Documentation

The landing page above plus the minimal examples are usually enough to evaluate the library. Everything below is
expanded, with examples, in [docs/FEATURES.md](docs/FEATURES.md) — dive in only when you need the specifics.

#### Core HTTP semantics

- [Routing patterns & path parameters](docs/FEATURES.md#routing-patterns--path-parameters)
- [Query String & Parameter Decoding](docs/FEATURES.md#query-string--parameters)
- [Trailing Slash Policy](docs/FEATURES.md#trailing-slash-policy)
- [Construction Model (RAII & Ephemeral Ports)](docs/FEATURES.md#construction-model-raii--ephemeral-ports)
- [HttpServer Lifecycle](docs/FEATURES.md#httpserver-lifecycle)
- [Reserved & Managed Headers](docs/FEATURES.md#reserved--managed-response-headers)
- [Request Header Duplicate Handling](docs/FEATURES.md#request-header-duplicate-handling-detailed)
- [Connection Close Semantics](docs/FEATURES.md#connection-close-semantics)
- [Streaming Responses (Chunked / Incremental)](docs/FEATURES.md#streaming-responses-chunked--incremental)
- [Mixed Mode & Dispatch Precedence](docs/FEATURES.md#mixed-mode--dispatch-precedence)

#### Middleware & content processing

- [Rate Limiting Middleware](docs/FEATURES.md#rate-limiting-middleware)
- [CORS Support](docs/FEATURES.md#cors-support)
- [Compression & Negotiation](docs/FEATURES.md#compression--negotiation)
- [Inbound Request Decompression](docs/FEATURES.md#inbound-request-decompression-config-details)
- [Multipart/form-data utilities](docs/FEATURES.md#multipartform-data-utilities-rfc-7578)
- [Static File Handler & Range Requests](docs/FEATURES.md#static-file-handler-rfc-7233--rfc-7232)

#### Protocols & observability

- [WebSocket](docs/FEATURES.md#websocket-rfc-6455)
- [HTTP/2](docs/FEATURES.md#http2-rfc-9113)
- [TLS Features](docs/FEATURES.md#tls-features)
- [Automatic HTTP → HTTPS redirect](docs/FEATURES.md#automatic-http--https-redirect)
- [OpenTelemetry Integration](docs/FEATURES.md#opentelemetry-integration)
- [Logging](docs/FEATURES.md#logging)

## Public objects and usage

Consuming `aeronet` will result in the client code interacting with [handler registration](#handler-registration) and [routing patterns](#routing-patterns--path-parameters), [server objects](#server-objects), [router](#router-configuration-two-safe-ways-to-set-handlers), [http responses](#building-the-http-response), streaming HTTP responses and reading HTTP requests.

### Handler registration

Two approaches:

1. Per method and path handlers: `router.setPath(http::Method::GET | http::Method::POST, "/hello", handler)` – exact path match.
1. Global handler: `router.setDefault([](const HttpRequest&){ ... })` catchs all requests not matched by path handlers.

Rules:

- If a default handler is not registered and a request does not match a path handler -> 404 Not Found.
- If path exists but method not allowed -> 405 Method Not Allowed.
- You can call `setPath` repeatedly on the same path to extend the allowed method mask (handler is replaced, methods merged).
- You can also call `setPath` once for several methods by using the `|` operator (for example: `http::Method::GET | http::Method::POST`)

Example:

```cpp
Router router;
router.setPath(http::Method::GET | http::Method::PUT, "/hello", [](const HttpRequest&){
  return HttpResponse(200).body("world");
});
router.setPath(http::Method::POST, "/echo", [](const HttpRequest& req){
  return HttpResponse(200).body(req.body());
});
// Add another method later (merges method mask, replaces handler)
router.setPath(http::Method::GET, "/echo", [](const HttpRequest& req){
  return HttpResponse(200).body("Echo via GET");
});
```

### Routing Patterns & Path Parameters

**Path Parameters**: Use `{name}` for named parameters or `{}` for unnamed (zero-indexed) parameters (but you cannot mix both named and unnamed in the same path):

You can also constrain a parameter inline with `{name:pattern}` (for example `/users/{id:[0-9]+}`) to reject non-matching segments during routing.

```cpp
Router router;
// Matches: /users/42/posts/hello
router.setPath(http::Method::GET, "/users/{id}/posts/{name}", [](const HttpRequest& req) { 
  std::string_view id = req.pathParamValueOrEmpty("id"); // "42"
  std::string_view name = req.pathParamValueOrEmpty("name"); // "hello"
  return HttpResponse(200); 
});
```

```cpp
Router router;
// Matches: /api/v3/search-something
router.setPath(http::Method::GET, "/api/v{}/search-{}", [](const HttpRequest& req) { 
  std::string_view version = req.pathParamValueOrEmpty("0"); // "3"
  std::string_view type = req.pathParamValueOrEmpty("1"); // "something"
  return HttpResponse(200); 
});
```

**Wildcard**: If the last segment is exactly `*`, it matches any remaining path segments:

```cpp
Router router;
router.setPath(http::Method::GET, "/files/*", [](const HttpRequest& req) { return HttpResponse(200); });
// Matches: /files/a.txt
// Matches: /files/a/b.txt
```

At any other position in the path, `*` is the literal asterisk character. Example:

```cpp
Router router;
router.setPath(http::Method::GET, "/glob/*/file.txt", [](const HttpRequest& req) { return HttpResponse(200); });  
// Matches: /glob/*/file.txt but not /glob/a/file.txt
```

**Escape Sequences**: To use literal special characters in paths, escape them:

- `{{` → literal `{`
- `}}` → literal `}`

```cpp
Router router;
router.setPath(http::Method::GET, "/api/{{version}}/data", [](const HttpRequest& req) { return HttpResponse(200); });
// Matches: /api/{version}/data (literal braces)
```

See [docs/FEATURES.md](docs/FEATURES.md#routing-patterns--path-parameters) for complete routing syntax.

### Server objects

`aeronet` provides 2 types of servers: `SingleHttpServer` and `MultiHttpServer`.
Client code will mostly use `MultiHttpServer` because it's the one supporting multi-threaded scaling out of the box, but `SingleHttpServer` is also available for simpler use cases or when the user wants to manage multiple server instances manually.
For convenience, a `HttpServer` alias is provided for `MultiHttpServer` which is the recommended default server type.
These are the main objects expected to be used by the client code.

#### SingleHttpServer

The core server of `aeronet`. It is a mono-threaded process based on a reactor pattern powered by `epoll` with a blocking running event loop.
The call to `run()` (or `runUntil(<predicate>)`) is blocking, and can be stopped by another thread by calling `stop()` on this instance.
The non-blocking APIs launch the event loop in the background. Use `start()` when you want a void convenience that manages an internal handle for you, or `startDetached()` (and the related `startDetachedAndStopWhen(<predicate>)`, `startDetachedWithStopToken(<stop token>)`) when you need an `AsyncHandle` you can inspect or control explicitly.

Key characteristics:

- It is a **RAII** class - and actually `aeronet` library as a whole does not have any singleton for a cleaner & expected design (except for signal handlers, but it's because signals themselves are global), so all resources linked to the `SingleHttpServer` are tied (and will be released with) it.
- It is **copyable** and **moveable** if and only if it is **not running**.
**Warning!** Unlike most C++ objects, the move operations are not `noexcept` to make sure that client does not move a running server (it would throw in that case, and only in that case). Moving a non-running `SingleHttpServer` is, however, perfectly safe and `noexcept` in practice.
- It is **restartable**, you can call `start()` after a `stop()`.
- You can modify most of its **configuration safely at runtime** via `postConfigUpdate()` and `postRouterUpdate()`.
- Graceful draining is available via `beginDrain(std::chrono::milliseconds maxWait = 0)`: it stops accepting new connections, lets in-flight responses finish with `Connection: close`, and optionally enforces a deadline before forcing the remaining connections to close.

##### Memory Management & std::string_view Safety

**aeronet**'s API extensively uses `std::string_view` for zero-copy performance. This is safe because each connection maintains its own buffer, and all `HttpRequest` data (path, query params, headers, body) consists of `std::string_view` instances pointing into this per-connection buffer. The buffer remains valid for the entire duration of the handler execution, making all request data safe to access without copies.

For detailed information about buffer lifetime guarantees and best practices (especially for coroutines), see [Memory Management & std::string_view Safety](docs/FEATURES.md#memory-management--stdstring_view-safety).

##### Configuration

All configuration of the `SingleHttpServer` is applied per **server instance** (the server **owns** its configuration).

`SingleHttpServer` takes a `HttpServerConfig` by value at construction, which allows full control over the server parameters (port, timeouts, limits, TLS setup, compression options, etc). Once constructed, some fields can be updated, even while the server is running thanks to `postConfigUpdate` method.

Note that `nbThreads` field should be 1 for `SingleHttpServer`. If you intend to use multiple threads, consider using `HttpServer` (aka `MultiHttpServer`) instead.

`HttpServerConfig` lives in `aeronet/http-server-config.hpp` and exposes fluent setters (withX naming):

```cpp
HttpServerConfig cfg;
cfg.withPort(8080)
  .withReusePort(true)
  .withMaxHeaderBytes(16 * 1024)
  .withMaxBodyBytes(2 * 1024 * 1024)
  .withKeepAliveTimeout(std::chrono::milliseconds{10'000})
  .withMaxRequestsPerConnection(500)
  .withKeepAliveMode(true);

SingleHttpServer server(std::move(cfg)); // or SingleHttpServer(8080) then server.setConfig(cfgWithoutPort);
```

###### Limits

- **431** is returned if the header section exceeds `maxHeaderBytes`.
- **413** is returned if the declared `content-length` exceeds `maxBodyBytes`.
- Connections exceeding `maxOutboundBufferBytes` (buffered pending write bytes) are marked to close after flush (default 4MB) to prevent unbounded memory growth if peers stop reading.
- Slowloris protection: configure `withHeaderReadTimeout(ms)` to bound how long a client may take to send an entire request head (request line + headers) (0 to disable). `aeronet` will return HTTP error **408 Request Timeout** if exceeded.

###### Performance / Metrics & Backpressure

`SingleHttpServer::stats()` exposes aggregated counters:

- `totalBytesQueued` – bytes accepted into outbound buffering (including those sent immediately)
- `totalBytesWrittenImmediate` – bytes written synchronously on first attempt (no buffering)
- `totalBytesWrittenFlush` – bytes written during later flush cycles (EPOLLOUT)
- `deferredWriteEvents` – number of times EPOLLOUT was registered due to pending data
- `flushCycles` – number of flush attempts triggered by writable events
- `maxConnectionOutboundBuffer` – high-water mark of any single connection's buffered bytes

Use these to gauge backpressure behavior and tune `maxOutboundBufferBytes`. When a connection's pending buffer would exceed the configured maximum, it is marked for closure once existing data flushes, preventing unbounded memory growth under slow-reader scenarios.

###### Metrics Callback (Scaffold)

You can install a lightweight per-request metrics callback capturing basic timing and size information:

```cpp
SingleHttpServer server;
server.setMetricsCallback([](const SingleHttpServer::RequestMetrics& m){
  // Export to stats sink / log
  // m.method, m.target, m.status, m.bytesIn, m.bytesOut (currently 0 for fixed responses), m.duration, m.reusedConnection
});
```

Current fields (alpha – subject to change before 1.0):

| Field | Description |
|-------|-------------|
| method | Original request method string |
| target | Request target (decoded path) |
| status | Response status code (best-effort 200 for streaming if not overridden) |
| bytesIn | Request body size (after chunk decode) |
| bytesOut | Placeholder (0 for now, future: capture flushed bytes per response) |
| duration | Wall time from parse completion to response dispatch end (best effort) |
| reusedConnection | True if this connection previously served other request(s) |

The callback runs in the event loop thread – keep it non-blocking.

#### Running an asynchronous event loop (non blocking)

A convenient set of methods on a `SingleHttpServer` that allow non blocking:

`start()` - non-blocking convenience (returns void); the server manages an internal handle.

`startDetached()` - non-blocking; returns an `AsyncHandle` giving explicit lifecycle control.

`startDetachedAndStopWhen(<predicate>)` - like `startDetached()` but stops when predicate fires.

`startDetachedWithStopToken(<stop token>)` - like `startDetached()` but integrates with `std::stop_token`.

These methods allow running the server in a background thread; pick `startDetached()` when you need the handle, or `start()` when you do not.

```cpp
#include <aeronet/aeronet.hpp>
using namespace aeronet;

int main() {
  Router router;
  router.setDefault([](const HttpRequest&){ return HttpResponse(200).body("hi"); });
  SingleHttpServer srv(HttpServerConfig{}, std::move(router));
  // Launch in background thread and capture lifetime handle
  auto handle = srv.startDetached();
  // main thread free to do orchestration / other work
  std::this_thread::sleep_for(std::chrono::seconds(2));
  handle.stop();
  handle.rethrowIfError();
}
```

Predicate form (stop when external flag flips):

```cpp
std::atomic<bool> done{false};
SingleHttpServer srv(HttpServerConfig{});
auto handle = srv.startDetachedAndStopWhen([&]{ return done.load(); });
// later
done = true; // loop exits soon (bounded by poll interval)
```

Stop-token form (std::stop_token):

```cpp
// If you already manage a std::stop_source you can pass its token directly
// to let the caller control the server lifetime via cooperative cancellation.
std::stop_source src;
SingleHttpServer srv(HttpServerConfig{});
auto handle = srv.startDetachedWithStopToken(src.get_token());
// later
src.request_stop();
```

Notes:

- Register handlers before `start()` unless you provide external synchronization for modifications.
- `stop()` is idempotent; destructor performs it automatically as a safety net.
- keep returned `AsyncHandle` to keep the server running; server will be stopped at its destruction.

#### HttpServer, aka MultiHttpServer, a multi threading version of SingleHttpServer

Instead of manually creating N threads and N `SingleHttpServer` instances, you can use `HttpServer` to spin up a "farm" of identical servers with same routing configuration, on the same port. It:

- Accepts a base `HttpServerConfig` (set `port=0` for ephemeral bind; the same chosen port is propagated to all instances)
- Replicates either a global handler or all registered path handlers across each underlying server (even after in-flight updates)
- Exposes `stats()` returning both per-instance and aggregated totals (sums; `maxConnectionOutboundBuffer` is a max)
- Provides the resolved listening `port()` directly after construction (even for ephemeral port 0 requests)
- Provides the same lifecycle APIs as `SingleHttpServer`: blocking `run()` / `runUntil(pred)`, non-blocking `start()` / `startDetached()`, `stop()`, `beginDrain()`, etc.
- Like `SingleHttpServer`, `HttpServer` is copyable and moveable when not running, and restartable after stop.

Example:

```cpp
#include <aeronet/aeronet.hpp>
#include <aeronet/log.hpp>
using namespace aeronet;

int main() {
  Router router;
  router.setDefault([](const HttpRequest& req){
    return HttpResponse(200).body("hello\n");
  });
  HttpServer multi(HttpServerConfig{}.withNbThreads(4), std::move(router)); // 4 underlying event loops
  multi.start();
  // ... run until external signal, or call stop() ...
  std::this_thread::sleep_for(std::chrono::seconds(30));
  auto agg = multi.stats();
  log::info("instances={} queued={}\n", agg.per.size(), agg.total.totalBytesQueued);
}
```

Additional notes:

- If `cfg.port` was 0 the kernel-chosen ephemeral port printed above will remain stable across any later `stop()` /
  `start()` cycles for this `HttpServer` instance. To obtain a new ephemeral port you must construct a new `HttpServer` (or in a future API explicitly reset the base configuration before a restart to `port=0`).
- You may call `stop()` and then `start()` again on the same `HttpServer` instance.
- Handlers: global or path handlers registered are re-applied to the fresh servers on each
  restart. You may add/remove/replace path handlers using `postRouterUpdate()` or `router()` at any time (even during running).
- Per‑run statistics are not accumulated across restarts; each run begins with fresh counters (servers rebuilt).

Stats aggregation example:

```cpp
#include <aeronet/log.hpp>

HttpServer multi(HttpServerConfig{}.withNbThreads(4), Router{});
auto st = multi.stats();
for (size_t i = 0; i < st.per.size(); ++i) {
  const auto& s = st.per[i];
  log::info("[srv{}] queued={} imm={} flush={}\n", i,
             s.totalBytesQueued,
             s.totalBytesWrittenImmediate,
             s.totalBytesWrittenFlush);
}
```

##### Example

```bash
./build/examples/aeronet-multi 8080 4   # port 8080, 4 threads
```

Each thread owns its own listening socket (`SO_REUSEPORT`) and epoll instance – no shared locks in the accept path.
This is the simplest horizontal scaling strategy before introducing a worker pool.

#### Summary table

| Variant | Header | Launch API | Blocking? | Threads Created | Scaling Model | Typical Use Case | Restartable? | Notes |
|---------|--------|------------|-----------|--------------------|---------------|------------------|--------------|-------|
| `SingleHttpServer` | `aeronet/single-http-server.hpp` | `run()` / `runUntil(pred)` | Yes (caller thread blocks) | 0 | Single reactor | Dedicated thread you manage or simple main-thread server | Yes | Minimal overhead, zero thread creation |
| `SingleHttpServer` | `aeronet/single-http-server.hpp` | `start()` (void convenience) / `startDetached()` / `startDetachedAndStopWhen(pred)` / `startDetachedWithStopToken(token)` | No (`startDetached()` returns `AsyncHandle`) | 1 `std::jthread` (owned by handle) | Single reactor (background) | Non-blocking single server, calling thread remains free | Yes | `startDetached()` returns RAII handle; `start()` is a void convenience |
| `HttpServer` | `aeronet/http-server.hpp` | `run()` / `runUntil(pred)` | Yes (caller thread blocks) | N (`threadCount`) | Horizontal `SO_REUSEPORT` multi-reactor | Multi-core throughput, blocking orchestration | Yes | All reactors run on caller thread until stop |
| `HttpServer` | `aeronet/http-server.hpp` | `start()` (void convenience) / `startDetached()` | No (`startDetached()` returns `AsyncHandle`) | N `std::jthread`s (internal) | Horizontal `SO_REUSEPORT` multi-reactor | Multi-core throughput, non-blocking launch | Yes | `startDetached()` returns RAII handle; `start()` is a void convenience |

Decision heuristics:

- Use `SingleHttpServer::run()` / `runUntil()` when you already own a thread (or can block `main()`) and want minimal abstraction with zero overhead.
- Use `SingleHttpServer::start()` family when you want a single server running in the background while keeping the calling thread free (e.g., integrating into a service hosting multiple subsystems, or writing higher-level control logic while serving traffic). The returned `AsyncHandle` provides RAII lifetime management with no added weight to `SingleHttpServer` itself.
- Use `HttpServer` when you need multi-core throughput with separate event loops per core – the simplest horizontal scaling path before introducing more advanced worker models.

Blocking semantics summary:

- `SingleHttpServer::run()` / `runUntil()` – fully blocking; returns only on `stop()` or when predicate is satisfied.
- `SingleHttpServer::start()` / `startDetachedAndStopWhen()` / `startDetachedWithStopToken()` – non-blocking; returns immediately with an `AsyncHandle`. Lifetime controlled via the handle's destructor (RAII) or explicit `handle.stop()`.
- `MultiHttpServer::run()` / `runUntil()` – fully blocking; returns only on `stop()` or when predicate is satisfied.
- `MultiHttpServer::start()` – non-blocking; returns after all reactors are launched, manages internal thread pool.

#### Signal-driven Shutdown (Process-wide)

`aeronet` provides a global signal handler mechanism for graceful shutdown of **all** running servers:

```cpp
// Install signal handlers for SIGINT/SIGTERM (typically in main before starting servers)
std::chrono::milliseconds maxDrainPeriod{5000}; // 5s max drain
SignalHandler::Enable(maxDrainPeriod);

// All SingleHttpServer instances regularly check for stop requests in their event loops
SingleHttpServer server(HttpServerConfig{});
server.run();  // Will drain and stop when SIGINT/SIGTERM received
```

Key points:

- **Process-wide**: `SignalHandler::Enable()` installs handlers that set a global flag checked by all `SingleHttpServer` instances (and so, `HttpServer` instances are also affected).
- **Automatic drain**: When a signal arrives, all running servers automatically call `beginDrain(maxDrainPeriod)` at the next event loop iteration.
- **Optional**: Don't call `SignalHandler::Enable()` if your application manages signals differently.

### Router configuration: two safe ways to set handlers

Routing configuration may be applied in two different ways depending on your application's lifecycle and threading model. Prefer pre-start configuration when possible; use the runtime proxy when you must mutate routing after server construction.

#### Pre-start configuration (recommended)

Construct and fully configure a `Router` instance on the calling thread, then pass it to the server constructor. This is the simplest and safest approach: router will be up to date immediately directly at server construction.

Example (recommended):

```cpp
Router router;
router.setPath(http::Method::GET, "/hello", [](const HttpRequest&){ return HttpResponse(200).body("hello"); });
SingleHttpServer server(HttpServerConfig{}, std::move(router));
server.run();
```

#### Runtime updates via `RouterUpdateProxy`

If you need to mutate routes while a server is active, use the `RouterUpdateProxy` exposed by `SingleHttpServer::router()` and by convenience  `HttpServer::router()`. The proxy accepts handler registration calls and forwards them to the server's event-loop thread so updates occur without racing the request processing. If the server is running, the update will be effective at most after one event polling period.

Example (runtime-safe):

```cpp
SingleHttpServer server(HttpServerConfig{});
auto handle = server.startDetached();
// later, from another thread:
server.router().setPath(http::Method::POST, "/upload", [](const HttpRequest&){ return HttpResponse(201); });
```

Notes:

- The proxy methods schedule updates to run on the server thread; they may execute immediately when the server is idle, or be queued and applied at the next loop iteration.
- The proxy will propagate exceptions thrown by your updater back to the caller when possible; handler registration conflicts (e.g. streaming vs non-streaming for same method+path) are reported.
- Prefer pre-start configuration for simpler semantics and testability; use runtime updates only when dynamic reconfiguration is required.

### Building the HTTP response

The router expects callback functions returning a `HttpResponse`.

You have two ways to construct a `HttpResponse`:

- Direct construction thanks to its numerous constructors taking status **code**, **body** & `content-type`, **headers**, additional capacity for headers/body/trailers
- [Optimized](#optimize-httpresponse-construction) construction from `HttpRequest::makeResponse()` that pre-applies server-global headers and other optimizations

You can build it thanks to the numerous provided methods to store the main components of a HTTP response (status code, reason, headers, body and trailers):

| Operation          | Complexity           | Notes                                  |
|--------------------|----------------------|----------------------------------------|
| `status()`         | O(1)                 | Overwrites 3 digits                    |
| `reason()`         | O(trailing)          | One tail `memmove` if size delta       |
| `header()`         | O(headers + bodyLen) | Linear scan + maybe one shift          |
| `headerAddLine()`      | O(bodyLen)           | Shift tail once; no scan               |
| `headerRemoveLine()`   | O(headers + bodyLen) | Linear scan (reverse) + maybe one shift          |
| `headerRemoveValue()`   | O(headers + bodyLen) | Linear scan (reverse) + maybe one shift          |
| `body()` (inline)  | O(delta) + realloc   | Exponential growth strategy            |
| `body()` (capture) | O(1)                 | Zero copy client buffer capture        |
| `bodyStatic()` (capture) | O(1)                 | Zero copy client buffer capture        |
| `bodyAppend()` (inline) | O(delta) + realloc   | Exponential growth strategy, zero-copy support            |
| `bodyInlineAppend()` | O(delta) + realloc   | Exponential growth strategy            |
| `bodyInlineSet()` | O(1) + realloc   | Exact growth strategy            |
| `file()`           | O(1)                 | Zero-copy sendfile helper              |
| `trailerAddLine()`     | O(1)                 | Append-only; no scan (only after body) |

Usage guidelines:

- Use `headerAddLine()` when duplicates are acceptable or not possible from the client code (cheapest path).
- Use `header()` only when you must guarantee uniqueness. Matching is case‑insensitive.
- Do not use any of those to set `Content-Type` and `Content-Length`. The first one is set along with the `body` methods, and the second one is managed by the library.
- Chain on temporaries for concise construction; the rvalue-qualified overloads keep the object movable.
- For maximum performance, fill the response in order, starting with status/reason, then headers, then body and trailers, to minimize memory shifts and reallocations.

#### Optimize HttpResponse construction

You can use `HttpRequest::makeResponse()` methods to optimize some job usually made at finalization time, directly at construction time.
This is especially useful when you have configured `globalHeaders` in the server config that you want to apply to all responses, as it avoids copying them again before the body (that would also shift the whole body, if inlined) at response finalization time.

Example:

```cpp
Router router;
router.setDefault([](const HttpRequest& req) {
  // Pre-applies global headers from server config
  return req.makeResponse("hello\n"); // response already contains global headers (for instance: 'server: aeronet')
});
```

Overloads make it possible to pass status and / or body & content-type, very useful for one-shot responses.

#### Reserved Headers

The library intentionally reserves a small set of response headers that user code cannot set directly on
`HttpResponse` (fixed responses) or via `HttpResponseWriter` (streaming) because aeronet itself manages them or
their semantics would be invalid / ambiguous without deeper protocol features:

Reserved now (assert if attempted in debug; ignored in release for streaming):

- `date` – generated once per second and injected automatically.
- `content-length` – computed from the body (fixed) or set through `contentLength()` (streaming). Prevents
  inconsistencies between declared and actual size.
- `connection` – determined by keep-alive policy (HTTP version, server config, request count, errors). User code
  supplying conflicting values could desynchronize connection reuse logic.
- `transfer-encoding` – controlled by streaming writer (`chunked`) or omitted when `content-length` is known. Allowing
  arbitrary values risks illegal CL + TE combinations or unsupported encodings.
- `trailer`, `te`, `upgrade` – not yet supported by aeronet; reserving them now avoids future backward-incompatible
  behavior changes when trailer / upgrade features are introduced.

Allowed convenience helpers:

- `content-type` via `contentType()` in streaming.
- `location` via `location()` for redirects.

##### Content-Type resolution for static files

When serving files with the built-in static helpers, aeronet chooses the response `content-type` using the
following precedence: (1) user-provided resolver callback if installed and non-empty, (2) the configured default
content type in `HttpServerConfig`, and (3) `application/octet-stream` as a final fallback. The `File::detectedContentType()`
helper is available for filename-extension based detection (the built-in mapping now includes common C/C++ extensions
such as `c`, `h`, `cpp`, `hpp`, `cc`).

All other headers (custom application / caching / CORS / etc.) may be freely set; they are forwarded verbatim.
This central rule lives in a single helper (`http::IsReservedResponseHeader`).

## Middleware & Cross-Cutting Features

### Rate Limiting Middleware

`aeronet` provides a middleware-first rate limiting API that can be attached globally, per-route, or through route
groups. Rejected requests receive `429 Too Many Requests` and a `Retry-After` header. It ships with an in-memory
token bucket by default; a client-library-agnostic Redis sliding-window adapter (behind `-DAERONET_ENABLE_REDIS=ON`)
is available for a distributed limit shared across multiple aeronet instances.

```cpp
Router router;
router.setPath(http::Method::GET, "/v1/data", [](const HttpRequest&) {
  return HttpResponse(200).body("ok");
});

// Uses InMemoryTokenBucketRateLimitStore automatically when `store` is unset.
router.addRequestMiddleware(RateLimitRequestMiddlewareBuilder{
  .config = RateLimitConfig{.requestsPerSecond = 50, .burst = 100},
  .keyStrategy = RateLimitClientKeyStrategy::PeerAddress
}.build());
```

See [Rate Limiting Middleware](docs/FEATURES.md#rate-limiting-middleware) for group-scoped limiters and the Redis
adapter contract (eval callback, key schema, script shape).

### Compression (gzip, deflate, zstd, brotli)

`aeronet` has built-in support for automatic outbound response compression and inbound requests decompression with multiple algorithms, provided that the library is built with each available encoder compile time flag.

Two compression layers for outbound responses:

- **Direct compression** compresses inline bodies at `body()` / `bodyAppend()` call time when using `HttpRequest::makeResponse()`. Controlled per-response via `DirectCompressionMode` (`Auto` / `Off` / `On`).
- **Finalization compression** applies at response finalization for bodies not already compressed.

Detailed negotiation rules, thresholds, opt-outs, and tuning have moved:
See: [Compression & Negotiation](docs/FEATURES.md#compression--negotiation)

Per-response manual override: setting any `Content-Encoding` (even `identity`) disables automatic compression for that
response. Details & examples: [Manual Content-Encoding Override](docs/FEATURES.md#per-response-manual-content-encoding-automatic-compression-suppression)

## Protocols & Modules

### HTTP/2 support

`aeronet` is compatible with HTTP/2, with or without TLS, when built with `-DAERONET_ENABLE_HTTP2=ON`.

When `AERONET_ENABLE_HTTP2` is OFF, the HTTP/2 module is not built and the HTTP/2-specific API surface (e.g. `Http2Config`, `HttpServerConfig::withHttp2()`) is not available.

HTTP/2 uses the same unified `HttpRequest` type as HTTP/1.1:

```cpp
#include <aeronet/aeronet.hpp>

using namespace aeronet;

int main() {
  Router router;
  
  // Single handler works for both HTTP/1.1 and HTTP/2
  router.setDefault([](const HttpRequest& req) {
    if (req.isHttp2()) {
      return HttpResponse{"Hello from HTTP/2! Stream: " + std::to_string(req.streamId()) + "\n"};
    }
    return HttpResponse{"Hello from HTTP/1.1\n"};
  });

  HttpServerConfig config;
  config.withPort(8443)
      .withTlsCertKey("server.crt", "server.key")
      .withTlsAlpnProtocols({"h2", "http/1.1"})
      .withHttp2(Http2Config{.enable = true});

  SingleHttpServer server(std::move(config), std::move(router));
  server.run();
}
```

Test: `curl -k --http2 https://localhost:8443/hello`

See the [full HTTP/2 example](examples/http2.cpp) for more details.

### HTTP Client

Although aeronet is primarily a server library, it ships an optional, lightweight **HTTP/1.1 + HTTP/2
client** (`aeronet::HttpClient`) built on the very same non-blocking transport, TLS, HPACK/frame-codec
and event-loop bricks as the server. Enable it with `-DAERONET_ENABLE_HTTP_CLIENT=ON` (on by default).
It is handy for service-to-service calls, health checks and tests that exercise a live server.

```cpp
#include <aeronet/http-client.hpp>

aeronet::HttpClient client;

// Simple GET (http or https). The result is an aeronet::HttpClientResult
// (std::expected<HttpResponse, HttpClientErrc>): the response on success, an error code otherwise.
auto result = client.get("https://example.com/health");
if (result) {
  const aeronet::HttpResponse& resp = *result;
  auto body = resp.bodyInMemory();                  // decoded body (chunked already de-framed)
  auto ctype = resp.headerValueOrEmpty("content-type");
} else {
  auto reason = aeronet::ErrcToStr(result.error());  // e.g. "connection failed", "operation timed out"
}

// POST with a JSON body and a custom header, via the fluent request builder
aeronet::ClientRequest req(aeronet::http::Method::POST, "https://example.com/api");
req.headerAddLine("X-Trace-Id", "abc123").body(R"({"key":"value"})", "application/json");
auto created = client.request(req);
if (created) {
  auto status = created->status();
}
```

Highlights:

- Plain HTTP and HTTPS (HTTPS requires `-DAERONET_ENABLE_OPENSSL=ON`; SNI + peer/hostname verification on by default, configurable via `HttpClientConfig`).
- **Native HTTP/2** (requires `-DAERONET_ENABLE_HTTP2=ON`, on by default), reusing the server's HPACK + frame codecs. `HttpClientConfig::httpVersion` selects the mode: `Auto` (the default — https negotiates `h2` via ALPN with an `http/1.1` fallback, plain http stays HTTP/1.1), `Http2` (require HTTP/2: ALPN `h2` only over https, prior-knowledge h2c over plain http) or `Http1_1` (never speak HTTP/2). The same `request()`/`get()`/`post()` API serves both protocols; a pooled HTTP/2 connection keeps its negotiated settings and HPACK tables across requests.
- Per-origin keep-alive connection pooling with a transparent retry on a stale pooled connection.
- **Forward proxy** support (`HttpClientConfig::withProxy`): route every request through a cleartext HTTP proxy — an https origin is reached by opening an HTTP `CONNECT` tunnel and handshaking TLS through it, a plain http origin is sent to the proxy in absolute-form. An optional CA bundle verifies an intercepting proxy that re-signs origin certificates (e.g. mitmproxy).
- `Content-Length`, chunked transfer-encoding and connection-close framing; automatic redirect following with method rewriting.
- **Automatic response decompression** (`gzip` / `deflate` / `br` / `zstd`, gated on compiled-in codecs) and optional **request body compression** for large payloads — both reuse the very same codec bricks as the server and decode without an extra copy of the compressed bytes. Configured via `HttpClientConfig::decompression` / `requestCompression` (mirroring the server's `DecompressionConfig` / `CompressionConfig`). When decompression is on, the client also auto-advertises the codecs it can decode in `Accept-Encoding`.
- Convenience verbs (`get` / `head` / `post` / `put` / `del`) plus the `ClientRequest` builder.
- Reuses `aeronet::HttpResponse` as the response/request field container (no bespoke header/body types).

```cpp
#include <aeronet/http-client.hpp>

aeronet::HttpClientConfig cfg;
cfg.withRequestCompression(aeronet::Encoding::zstd);  // compress big outbound bodies (opt-in)
// Response decompression is on by default whenever a codec is compiled in.
aeronet::HttpClient client(cfg);
auto result = client.post("https://example.com/api", R"({"key":"value"})", "application/json");
if (result) {
  // result->bodyInMemory() is already decoded; the Content-Encoding header has been dropped.
  auto body = result->bodyInMemory();
}
```

Route every request through a cleartext HTTP forward proxy. For an https origin the client opens an HTTP
`CONNECT` tunnel to the origin and completes the TLS handshake through it; a plain http origin is sent to
the proxy in absolute-form. The optional second argument is a CA bundle used to verify an intercepting
proxy that re-signs origin certificates (mitmproxy and friends). A malformed proxy URL or an `https` proxy
throws `HttpClientException` at construction; a proxy that refuses the `CONNECT` yields
`HttpClientErrc::proxyError`.

```cpp
#include <aeronet/http-client.hpp>

aeronet::HttpClientConfig cfg;
cfg.withProxy("http://127.0.0.1:8080");  // or withProxy(url, "/etc/mitmproxy/ca.pem") for an intercepting proxy
aeronet::HttpClient client(cfg);
auto result = client.get("https://example.com/health");  // reached through a CONNECT tunnel
```

The returned response is an `HttpResponse` — a `using` alias of the generic single-buffer `HttpMessage`
type. Received headers are surfaced losslessly (reserved headers such as `Connection` / `Date` are
stored verbatim via `HttpMessage::rawHeader()`; only `Content-Type` / `Content-Length` /
`Transfer-Encoding` are normalized through the body and de-framing).

Every request returns an `aeronet::HttpClientResult` (`std::expected<HttpResponse, HttpClientErrc>`): a non-2xx status is a normal `HttpResponse` in the success state, while a per-request runtime failure (invalid URL, DNS/connect failure, timeout, TLS handshake error, malformed/oversized response, ...) lands in the error state as an `HttpClientErrc` — none of these throw. `ErrcToStr()` maps a code to a description. Exceptions are reserved for hard setup errors detected while building the client / TLS context (codec or certificate misconfiguration): those throw `aeronet::HttpClientException` (or `std::logic_error` when `https` is requested in a build without OpenSSL). Received headers are surfaced losslessly: `Content-Type` and the decoded `Content-Length` are normalized through the body, `Transfer-Encoding` is consumed while de-framing, and every other header (including `Connection`, `Date`, custom `X-*`, ...) is available verbatim via `headerValueOrEmpty()`.

> The current client is synchronous (it owns and drives its own event loop), so an HTTP/2 connection
> carries one stream at a time. A coroutine-friendly API (`co_await client.get(...)`) integrated with a
> running server loop — which would unlock true HTTP/2 multiplexing — is tracked in
> [docs/ROADMAP.md](docs/ROADMAP.md).

### JWT (JSON Web Tokens)

An optional **JWT** module (`aeronet::Jwt`) implements the JWS (signature) profile of RFC 7519 on top of
the OpenSSL crypto already linked for TLS — so it adds no new dependency. Enable it with
`-DAERONET_ENABLE_JWT=ON` (on by default whenever OpenSSL + glaze are enabled). JWE (encryption) is out
of scope.

```cpp
#include <aeronet/jwt.hpp>

// Sign with an HMAC secret (HS256). Keys can also come from a PEM RSA/EC/Ed25519
// key (aeronet::JwtKey::FromPem) or a JWK (aeronet::JwtKey::FromJwk).
aeronet::JwtKey key = aeronet::JwtKey::Hmac("super-secret-signing-key");
std::string token = aeronet::Jwt::encode(R"({"sub":"alice","exp":4102444800})", key,
                                         aeronet::JwtAlgorithm::HS256);

// Verify: signature + claim checks. The module is exception-free — failures surface as an
// invalid key, an empty token, or a JwtError (never thrown).
aeronet::JwtVerifyOptions opts;
opts.allowedAlgorithms = aeronet::JwtAlgorithmSet{aeronet::JwtAlgorithm::HS256};
aeronet::JwtError err = aeronet::JwtError::None;
if (aeronet::DecodedJwt decoded = aeronet::Jwt::tryDecode(token, key, opts, err)) {
  std::string_view sub = decoded.subject();  // "alice" (empty view == claim absent)
}
```

Highlights:

- Full JWS algorithm suite — HMAC `HS256/384/512`, RSA `RS*` / `PS*`, ECDSA `ES256/384/512`, `EdDSA` (Ed25519).
- Claim validation: `exp` / `nbf` (with `leeway` and an injectable `clock`), `iss` / `aud` / `sub`; `aud` as a string or an array.
- Keys from a shared secret, a PEM key, or a JWK; `aeronet::Jwks` parses a JWKS document and selects the verifying key by `kid` — pairs naturally with the HTTP client to fetch an issuer's keys.
- **Security by construction**: the unsecured `alg:none` is always rejected, a key whose family doesn't match the token `alg` is refused (the RS256↔HS256 confusion is structurally impossible), the signature is verified before any claim is parsed, and HMAC comparison is constant-time.
- Exception-free and opt-in (`-DAERONET_ENABLE_JWT`); see [docs/FEATURES.md](docs/FEATURES.md) for the full reference.

### OpenTelemetry Support (Experimental)

Optional distributed tracing & metrics integration. Enable with the CMake flag `-DAERONET_ENABLE_OPENTELEMETRY=ON`
(pulls in `protobuf` as an additional dependency). Each `SingleHttpServer` owns its own `TelemetryContext`
instance — no global singletons or static state, so multiple servers can run independent telemetry
configurations without interference, and every telemetry failure is logged via `log::error()` rather than
silently swallowed. You may also create your own `TelemetryContext` for custom metrics/traces.

See [OpenTelemetry Integration](docs/FEATURES.md#opentelemetry-integration) for the configuration API, built-in
instrumentation, and testing/observability notes.

## Operational Features

### Kubernetes style probes

Enable the builtin probes via `HttpServerConfig` and test them with curl. This example enables the probes with default paths and a plain-text content type.

```cpp
#include <aeronet/aeronet.hpp>

using namespace aeronet;

int main() {
  HttpServerConfig cfg;
  cfg.withBuiltinProbes(BuiltinProbesConfig{});

  Router router;
  // Register application handlers as usual (optional)
  router.setPath(http::Method::GET, "/hello", [](const HttpRequest&){
    return HttpResponse(200).body("hello\n");
  });

  SingleHttpServer server(std::move(cfg), std::move(router));

  server.run();
}
```

Probe checks (from the host/container):

```bash
curl -i http://localhost:8080/livez   # expects HTTP/1.1 200 when running
curl -i http://localhost:8080/readyz  # expects 200 when ready, 503 during drain/startup
curl -i http://localhost:8080/startupz # returns 503 until initialization completes
```

For Kubernetes deployment examples (ConfigMap + probes + full manifests), see: [docs/kubernetes-examples.md](docs/kubernetes-examples.md).

You can use `aeronet-config-dump` example to generate a baseline config file with all the default values, which you can then customize for your deployment needs (e.g. enabling TLS, tuning timeouts, etc.):

```bash
# Generate a baseline full config file (server + router)
./aeronet-config-dump --format yaml --output server.yaml
```

### Zero copy / Sendfile

There is a small example demonstrating `file` in `examples/aeronet-sendfile`.
It exposes two endpoints:

- `GET /static` - returns the contents of a file using `HttpResponse::file` (fixed response).
- `GET /stream` - returns the contents of a file using `HttpResponseWriter::file` (streaming writer API).

Build the examples and run the sendfile example:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/examples/aeronet-sendfile 8080 /path/to/file
```

If the file path argument is omitted the example creates a small temp file in `/tmp` and serves it.

Fetch the file with curl:

```bash
curl -i http://localhost:8080/static
curl -i http://localhost:8080/stream
```

The example demonstrates both the fixed-response (server synthesizes a `content-length` header) and the
streaming writer path. For plaintext sockets the server uses the kernel `sendfile(2)` syscall for zero-copy
transmission. When TLS is enabled the example exercises the TLS fallback that pread()s into the connection buffer
and writes through the TLS transport.

## Platform Support

| Platform | Status | I/O backend | Notes |
|----------|--------|-------------|-------|
| **Linux** (x86_64, aarch64) | Full support | epoll (edge-triggered) | Primary platform; all features including kTLS, `MSG_ZEROCOPY`, `sendfile`. Tested on **Ubuntu** and **Alpine** in the CI. |
| **macOS** (Apple Silicon / x86_64) | Supported | kqueue | Core HTTP/WebSocket server; Linux-specific optimizations auto-disabled |
| **Windows** (x64, MSVC) | Supported | WSAPoll | Core HTTP/WebSocket server; Linux-specific optimizations auto-disabled |

Linux-only features (gracefully disabled on other platforms):

- Kernel TLS (`kTLS`) and `sendfile(2)` for zero-copy file serving
- `MSG_ZEROCOPY` for large payload sends
- `eventfd` / `timerfd` for internal event loop signaling
- DogStatsD via Unix domain sockets

## Build & Installation

Full, continually updated build, install, and package manager instructions live in [`docs/INSTALL.md`](docs/INSTALL.md).

Quick start (release build of examples):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

For TLS toggles, sanitizers, Conan/vcpkg usage and `find_package` examples, see the INSTALL guide.

### C++ modules support (experimental)

When `AERONET_BUILD_MODULES=ON` is set in CMake, the library builds an optional C++20 module interface (`aeronet`).
The module re-exports the public API surface of `<aeronet/aeronet.hpp>` - only user-facing types, configuration
structs, handler callbacks, and protocol enums are exported; internal implementation details are omitted.

**Requirements:**

- CMake 3.28+
- A build system with C++20 module support (e.g. Ninja 1.11+)
- Compiler minimum versions: GCC 15, Clang 18, or MSVC 17.6

```cmake
cmake -B build -G Ninja -DAERONET_BUILD_MODULES=ON
cmake --build build
```

```cpp
#include <utility> // std::move - standard library is not exported by the module

import aeronet;

using aeronet::HttpRequest;
using aeronet::HttpResponse;
using aeronet::HttpServer;
using aeronet::HttpServerConfig;
using aeronet::Router;
using aeronet::http::Method;

int main() {
    Router router;
    router.setPath(Method::GET, "/hello", [](const HttpRequest& req) {
        return HttpResponse(200).header("X-Req-Body", req.body()).body("hello from aeronet\n");
    });
    HttpServer server(HttpServerConfig{}, std::move(router));
    server.run();
}
```

### Testing

The test suite uses a unified helper for simple GETs, streaming incremental reads, and multi-request keep-alive batches. See `docs/test-client-helper.md` for guidance when adding new tests.

## Test Coverage Matrix

Summary of current automated test coverage (see `tests/` directory). Legend: ✅ covered by explicit test(s), ⚠ partial / indirect, ❌ not yet.

| Area | Feature | Test Status | Notes / Representative Test Files |
|------|---------|-------------|-----------------------------------|
| Parsing | Request line (method/target/version) | ✅ | `http-parser-errors_test.cpp`, `http-core_test.cpp` |
| Parsing | Unsupported HTTP version (505) | ✅ | `http-parser-errors_test.cpp` |
| Parsing | Header parsing & lookup | ✅ | `http-core_test.cpp`, `http-core_test.cpp` |
| Limits | Max header size -> 431 | ✅ | `http-core_test.cpp` |
| Limits | Max body size (Content-Length) -> 413 | ✅ | `http-additional_test.cpp` |
| Limits | Chunk total/body growth -> 413 | ✅ | exercised across `http-chunked-head_test.cpp` and parser fuzz paths |
| Bodies | Content-Length body handling | ✅ | `http-core_test.cpp`, `http-additional_test.cpp` |
| Bodies | Chunked decoding | ✅ | `http-chunked-head_test.cpp`, `http-parser-errors_test.cpp` |
| Bodies | Trailers exposure | ✅ | Implemented (see tests/http-trailers_test.cpp) |
| Expect | 100-continue w/ non-zero length | ✅ | `http-parser-errors_test.cpp` |
| Expect | No 100 for zero-length | ✅ | `http-parser-errors_test.cpp`, `http-additional_test.cpp` |
| Keep-Alive | Basic keep-alive persistence | ✅ | `http-core_test.cpp` |
| Keep-Alive | Max requests per connection | ✅ | `http-additional_test.cpp`|
| Keep-Alive | Idle timeout close | ⚠ | Indirectly covered; explicit idle-time tests are planned |
| Pipelining | Sequential pipeline of requests | ✅ | `http-additional_test.cpp` |
| Pipelining | Malformed second request handling | ✅ | `http-additional_test.cpp` |
| Methods | HEAD semantics (no body) | ✅ | `http-chunked-head_test.cpp`, `http-additional_test.cpp` |
| Date | RFC7231 format + correctness | ✅ | `http-core_test.cpp` |
| Date | Same-second caching invariance | ✅ | `http-core_test.cpp` |
| Date | Second-boundary refresh | ✅ | `http-core_test.cpp` |
| Errors | 400 Bad Request (malformed line) | ✅ | `http-core_test.cpp` |
| Parsing | Percent-decoding of path | ✅ | `http-url-decoding_test.cpp`, `http-query-parsing_test.cpp` |
| Errors | 431, 413, 505, 501 | ✅ | `http-core_test.cpp`, `http-additional_test.cpp` |
| Errors | PayloadTooLarge in chunk decoding | ⚠ | Exercised indirectly; dedicated test planned |
| Concurrency | `SO_REUSEPORT` distribution | ✅ | `multi-http-server_test.cpp` |
| Lifecycle | Move semantics of server | ✅ | `http-server-lifecycle_test.cpp` |
| Lifecycle | Graceful stop (runUntil) | ✅ | many tests use runUntil patterns |
| Diagnostics | Parser error callback (version, bad line, limits) | ✅ | `http-parser-errors_test.cpp` |
| Diagnostics | PayloadTooLarge callback (Content-Length) | ⚠ | Indirect; explicit capture test planned |
| Performance | Date caching buffer size correctness | ✅ | covered by `http-core_test.cpp` assertions |
| Performance | writev header+body path | ⚠ | Indirectly exercised; no direct assertion yet |
| TLS | Handshake & rejection behavior | ✅ | `http-tls-handshake_test.cpp`, `http-tls-io_test.cpp` |
| Streaming | Streaming response & incremental flush | ✅ | `http-streaming_test.cpp` |
| Routing | Path & method matching | ✅ | `http-routing_test.cpp`, `router_test.cpp` |
| Compression | Negotiation & outbound insertion | ✅ | `http-compression_test.cpp`, `http-request-decompression_test.cpp` |
| OpenTelemetry | Basic integration smoke | ✅ | `opentelemetry-integration_test.cpp` |
| Async run | SingleHttpServer::start() behavior | ✅ | `http-server-lifecycle_test.cpp` |
| Misc / Smoke | Probes, stats, misc invariants | ✅ | `http-server-lifecycle_test.cpp`, `http-stats_test.cpp` |
| Implemented | Trailers (outgoing chunked / trailing headers) | ✅ | See tests/http-trailers_test.cpp and http-response-writer.hpp |

## Acknowledgements

Compression libraries (zlib, zlib-ng, zstd, brotli), OpenSSL, Opentelemetry and spdlog provide the optional feature foundation; thanks to their maintainers & contributors.

This project also includes code from the following open source projects:

- [amc](https://github.com/amadeusitgroup/amc), licensed under the MIT License.
- [flat_hash_map](https://github.com/skarupke/flat_hash_map), no license.
- [CityHash](https://github.com/google/cityhash), licensed under the MIT License.

## License

Licensed under the MIT License. See [LICENSE](LICENSE).
