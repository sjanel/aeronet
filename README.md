# aeronet

![aeronet Logo](resources/logo.png)

[![CI](https://github.com/sjanel/aeronet/actions/workflows/ci.yml/badge.svg)](https://github.com/sjanel/aeronet/actions/workflows/ci.yml)
[![Packaging](https://github.com/sjanel/aeronet/actions/workflows/packaging.yml/badge.svg)](https://github.com/sjanel/aeronet/actions/workflows/packaging.yml)
[![clang-format](https://github.com/sjanel/aeronet/actions/workflows/clang-format-check.yml/badge.svg)](https://github.com/sjanel/aeronet/actions/workflows/clang-format-check.yml)

**aeronet** is a modern, fast, modular and ergonomic HTTP/1.1 C++ server library for Linux focused on predictable performance, explicit control and minimal dependencies.

## Key Benefits

- Fast & predictable: edge‑triggered reactor model, zero/low‑allocation hot paths, horizontal scaling with port reuse.
- Safe by default: strict parsing, size/time guards, optional TLS & compression with defensive limits.
- Modular & opt‑in: enable only the features you need (zlib, zstd, brotli, TLS, logging, opentelemetry) via build flags (no more bloat dependencies)
- Ergonomic: simple `HttpServer`, `AsyncHttpServer`, `MultiHttpServer` types; fluent configuration; RAII listener setup.
- Extensible & observable: composable configs (compression, decompression, TLS) plus lightweight per‑request metrics hook.

## Minimal Example

Spin up a basic HTTP/1.1 server that responds on `/hello` in just a few lines. If you pass `0` as the port (or omit it), the kernel picks an ephemeral port which you can query immediately.

```cpp
#include <aeronet/aeronet.hpp>

using namespace aeronet;

int main() {
  HttpServer server(HttpServerConfig{}); // no specified port, OS will pick a free one
  server.router().setPath("/hello", http::Method::GET, [](const HttpRequest&) {
    return HttpResponse(200, "OK").contentType("text/plain").body("hello from aeronet\n");
  });
  server.run(); // blocking
}
```

## Quick Start with provided examples

Minimal server examples are provided in [examples](examples) directory.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/examples/aeronet-minimal 8080   # or omit 8080 for ephemeral
```

Test with curl:

```bash
curl -i http://localhost:8080/hello

HTTP/1.1 200
Content-Length: 151
Connection: keep-alive
Date: Sun, 05 Oct 2025 09:08:14 GMT

Hello from aeronet minimal server! You requested /hello
Method: GET
Version: HTTP/1.1
Headers:
Accept: */*
Host: localhost:8080
User-Agent: curl/8.5.0
```

## Detailed Documentation

The following focused docs expand each area without cluttering the high‑level overview:

- [Feature reference (FEATURES)](docs/FEATURES.md)

- [Compression & Negotiation](docs/FEATURES.md#compression--negotiation)
- [Inbound Request Decompression](docs/FEATURES.md#inbound-request-decompression-config-details)
- [Connection Close Semantics](docs/FEATURES.md#connection-close-semantics)
- [Reserved & Managed Headers](docs/FEATURES.md#reserved--managed-response-headers)
- [Query String & Parameter Decoding](docs/FEATURES.md#query-string--parameters)
- [Trailing Slash Policy](docs/FEATURES.md#trailing-slash-policy)
- [MultiHttpServer Lifecycle](docs/FEATURES.md#multihttpserver--lifecycle)
- [TLS Features](docs/FEATURES.md#tls-features)

If you are evaluating the library, the feature highlights above plus the minimal example are usually sufficient. Dive into the docs only when you need specifics (e.g. multi‑layer decompression safety rules or ALPN strict mode behavior).

## Feature Matrix (Concise)

| Category | Implemented (✔) | Notes |
|----------|-----------------|-------|
| Core HTTP/1.1 parsing | ✔ | Request line, headers, chunked bodies, pipelining |
| Routing | ✔ | Exact path + method allow‑lists; streaming + fixed |
| Keep‑Alive / Limits | ✔ | Header/body size, max requests per connection, idle timeout |
| Compression (gzip/deflate/zstd/br) | ✔ | Flags opt‑in; q‑value negotiation; threshold; per‑response opt‑out |
| Inbound body decompression | ✔ | Multi‑layer, safety guards, header removal |
| TLS | ✔ (flag) | ALPN, mTLS (optional/required), timeouts, metrics |
| OpenTelemetry | ✔ (flag) | Distributed tracing spans, metrics counters (experimental) |
| Async wrapper | ✔ | Background thread convenience |
| Metrics hook | ✔ (alpha) | Per‑request basic stats |
| Logging | ✔ (flag) | spdlog optional |
| Duplicate header policy | ✔ | Deterministic, security‑minded |
| Trailers exposure | ✖ | Planned |
| Middleware helpers | ✖ | Planned |
| Streaming inbound decompression | ✖ | Planned |
| sendfile / static file helper | ✖ | Planned |

## Core HTTP & Protocol Features (Implemented)

## Developer / Operational Features

| Feature | Notes |
|---------|-------|
| Epoll edge-triggered loop | One thread per `HttpServer`; writev used for header+body scatter-gather |
| SO_REUSEPORT scaling | Horizontal multi-reactor capability |
| Multi-instance wrapper | `MultiHttpServer` orchestrates N reactors, aggregates stats (explicit `reusePort=true` required for >1 threads; port resolved at construction) |
| Async single-server wrapper | `AsyncHttpServer` runs one server in a background thread |
| Move semantics | Transfer listening socket & loop state safely |
| Restarts | All `HttpServer`, `AsyncHttpServer` and `MultiHttpServer` can be started again after stop |
| Heterogeneous lookups | Path handler map accepts `std::string`, `std::string_view`, `const char*` |
| Outbound stats | Bytes queued, immediate vs flush writes, high-water marks |
| Lightweight logging | Pluggable design (spdlog optional); ISO 8601 UTC timestamps |
| Builder-style config | Fluent `HttpServerConfig` setters (`withPort()`, etc.) |
| Metrics callback (alpha) | Per-request timing & size scaffold hook |
| RAII construction | Fully listening after constructor (ephemeral port resolved immediately) |
| Comprehensive tests | Parsing, limits, streaming, mixed precedence, reuseport, move semantics, keep-alive |
| Mixed handlers example | Normal + streaming coexistence on same path (e.g. GET streaming, POST fixed) |

The sections below provide a more granular feature matrix and usage examples.

## HTTP/1.1 Feature Matrix

Moved out of the landing page to keep things concise. See the full, continually updated matrices in:

- [HTTP/1.1 Feature Matrix](docs/FEATURES.md#http11-feature-matrix)
- [Performance / architecture](docs/FEATURES.md#performance--architecture)

## Main objects

Consuming `aeronet` will result in the client code interacting with [server objects](#server-objects), [http responses](#building-the-http-response), streaming HTTP responses (documentation TODO) and reading HTTP requests.

### Server objects

`aeronet` provides 3 types of servers: `HttpServer`, `AsyncHttpServer` and `MultiHttpServer`. These are the main objects expected to be used by the client code.

#### HttpServer

The core server of `aeronet`. It is mono-threaded process based on a reactor pattern powered by `epoll` with a blocking running event loop.
The call to `run()` (or `runUntil(<predicate>)`) is blocking, and can be stopped by another thread by calling `stop()` on this instance.

Key characteristics:

- It is a **RAII** class - and actually `aeronet` library as a whole does not have any singleton for a cleaner & expected design, so all resources linked to the `HttpServer` are tied (and will be released with) it.
- It is **not copyable**, but it is **moveable** if and only if it is **not running**.
**Warning!** Unlike most C++ objects, the move operations are not `noexcept` to make sure that client does not move a running server (it would throw in that case, and only in that case). Moving a non-running `HttpServer` is, however, perfectly safe and `noexcept` in practice.
- It is **restartable**, you can call `start()` after a `stop()`. You can modify the routing configuration before the new `start()`.
- It is also [trivially relocatable](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p1144r10.html), although I doubt it will be very useful for this type.

#### AsyncHttpServer, an asynchronous (non-blocking) HttpServer

A convenient wrapper of a `HttpServer` and a `std::jthread` object with non blocking `start()` (and `startAndStopWhen(<predicate>)`) and `stop()` methods.

```cpp
#include <aeronet/aeronet.hpp>
using namespace aeronet;

int main() {
  AsyncHttpServer async(HttpServerConfig{});
  server.server().router().setDefault([](const HttpRequest&){ return HttpResponse(200, "OK").body("hi").contentType("text/plain"); });
  async.start();
  // main thread free to do orchestration / other work
  std::this_thread::sleep_for(std::chrono::seconds(2));
  async.stop();
  async.rethrowIfError();
}
```

Predicate form (stop when external flag flips):

```cpp
std::atomic<bool> done{false};
AsyncHttpServer async(HttpServerConfig{});
async.startAndStopWhen([&]{ return done.load(); });
// later
done = true; // loop exits soon (bounded by poll interval)
async.stop();
```

Stop-token form (std::stop_token):

```cpp
// If you already manage a std::stop_source you can pass its token directly
// to let the caller control the server lifetime via cooperative cancellation.
std::stop_source src;
async.startWithStopToken(src.get_token());
// later
src.request_stop();
async.stop();
```

Notes:

- Register handlers before `start()` unless you provide external synchronization for modifications.
- `stop()` is idempotent; destructor performs it automatically as a safety net.

Handler rules mirror `HttpServer`:

- Routing should be configured before `start()`.
- Mixing global handler with path handlers is rejected.
- After `start()`, further registration throws.

Stats aggregation example:

```cpp
auto st = multi.stats();
for (size_t i = 0; i < st.per.size(); ++i) {
  const auto& s = st.per[i];
  std::print("[srv{}] queued={} imm={} flush={}\n", i,
             s.totalBytesQueued,
             s.totalBytesWrittenImmediate,
             s.totalBytesWrittenFlush);
}
```

#### MultiHttpServer, a multi threading version of HttpServer

Instead of manually creating N threads and N `HttpServer` instances, you can use `MultiHttpServer` to spin up a "farm" of identical servers with same routing configuration, on the same port. It:

- Accepts a base `HttpServerConfig` (set `port=0` for ephemeral bind; the chosen port is propagated to all instances)
- Forces `reusePort=true` automatically when thread count > 1
- Replicates either a global handler or all registered path handlers across each underlying server
- Exposes `stats()` returning both per-instance and aggregated totals (sums; `maxConnectionOutboundBuffer` is a max)
- Manages lifecycle with internal `std::jthread`s; `stop()` requests shutdown of every instance
- Provides the resolved listening `port()` after start (even for ephemeral port 0 requests)

Example:

```cpp
#include <aeronet/aeronet.hpp>
#include <print>
using namespace aeronet;

int main() {
  HttpServerConfig cfg; cfg.reusePort = true; // ephemeral, auto-propagated
  MultiHttpServer multi(cfg, 4); // 4 underlying event loops
  multi.router().setDefault([](const HttpRequest& req){
    return HttpResponse(200, "OK").body("hello\n").contentType("text/plain");
  });
  multi.start();
  // ... run until external signal, or call stop() ...
  std::this_thread::sleep_for(std::chrono::seconds(30));
  auto agg = multi.stats();
  std::print("instances={} queued={}\n", agg.per.size(), agg.total.totalBytesQueued);
}
```

Additional notes:

- If `cfg.port` was 0 the kernel-chosen ephemeral port printed above will remain stable across any later `stop()` /
  `start()` cycles for this `MultiHttpServer` instance.
- You may call `stop()` and then `start()` again on the same `MultiHttpServer` instance.
- Ephemeral port reuse: if the initial construction used `port=0`, the kernel-chosen port is stored in the resolved
  config and that SAME port is reused for all subsequent restarts by default. Restarts do not request a new ephemeral
  port automatically. (Design choice: stable port across cycles is usually what supervisors / load balancers expect.)
  To obtain a new ephemeral port you must construct a new `MultiHttpServer` (or in a future API explicitly reset
  the base configuration before a restart to `port=0`).
- Handlers: global or path handlers registered before the first `start()` are re-applied to the fresh servers on each
  restart. You may add/remove/replace path handlers and/or the global handler while the server is STOPPED (i.e. after
  `stop()` and before the next `start()`). Modification while running is still forbidden.
- Per‑run statistics are not accumulated across restarts; each run begins with fresh counters (servers rebuilt).
- You may modify or add path handlers (and/or replace the global handler) after `stop()` and before the next
  `start()`; attempting to do so while running throws.

##### Example

```bash
./build/examples/aeronet-multi 8080 4   # port 8080, 4 threads
```

Each thread owns its own listening socket (SO_REUSEPORT) and epoll instance – no shared locks in the accept path.
This is the simplest horizontal scaling strategy before introducing a worker pool.

#### Choosing Between HttpServer, AsyncHttpServer, and MultiHttpServer

| Variant | Header | Launch API | Blocking? | Threads Created Internally  | Scaling Model | Typical Use Case | Restartable? | Notes |
|---------|--------|------------|-----------|-----------------------------|---------------|------------------|--------------|-------|
| `HttpServer` | `aeronet/http-server.hpp` | `run()` / `runUntil(pred)`, `stop()` (called from another thread) | Yes (caller thread blocks) | 0 | Single reactor | Dedicated thread you manage or simple main-thread server | Yes |Minimal overhead |
| `AsyncHttpServer` | `aeronet/async-http-server.hpp` | `start()`, `startAndStopWhen(pred)`, `stop()` | No | 1 `std::jthread` | Single reactor (owned) | Need non-blocking single server with safe lifetime | Yes | Owns `HttpServer` internally |
| `MultiHttpServer` | `aeronet/multi-http-server.hpp` | `start()`, `stop()` | No | N (`threadCount`) | Horizontal SO_REUSEPORT multi-reactor | Scale across cores quickly | Yes | Replicates handlers pre-start |

Decision heuristics:

- Use `HttpServer` when you already own a thread (or can just block main) and want minimal abstraction.
- Use `AsyncHttpServer` when you want a single server but need the calling thread free (e.g. integrating into a service hosting multiple subsystems, or writing higher-level control logic while serving traffic).
- Use `MultiHttpServer` when you need multi-core throughput with separate event loops per core; simplest horizontal scale path before more advanced worker models.

Blocking semantics summary:

- `HttpServer::run()` / `runUntil()` – fully blocking; returns only on stop/predicate.
- `AsyncHttpServer::start()` – non-blocking; lifecycle controlled via stop token + `server.stop()`.
- `MultiHttpServer::start()` – non-blocking; returns after all reactors launched.

### Building the HTTP response

The router expects callback functions returning a `HttpResponse`. You can build it thanks to the numerous provided methods to store the main components of a HTTP 1 response (status code, reason, headers and body):

| Operation          | Complexity | Notes |
|--------------------|------------|-------|
| `statusCode()`     | O(1)       | Overwrites 3 digits |
| `reason()`         | O(trailing) | One tail `memmove` if size delta |
| `addCustomHeader()`| O(bodyLen) | Shift tail once; no scan |
| `customHeader()`   | O(headers + bodyLen) | Linear scan + maybe one shift |
| `body()`           | O(delta) + realloc | Exponential growth strategy |

Usage guidelines:

- Use `addCustomHeader()` when duplicates are acceptable or not possible from the client code (cheapest path).
- Use `customHeader()` only when you must guarantee uniqueness. Matching is case‑insensitive; prefer a canonical style (e.g.
  `Content-Type`) for readability, but behavior is the same regardless of input casing.
- Chain on temporaries for concise construction; the rvalue-qualified overloads keep the object movable.
- Finalize exactly once right before sending.

Future possible extensions (not yet implemented): transparent compression insertion, zero‑copy file send mapping,
and an alternate layout for extremely large header counts.

#### Reserved Headers

The library intentionally reserves a small set of response headers that user code cannot set directly on
`HttpResponse` (fixed responses) or via `HttpResponseWriter` (streaming) because aeronet itself manages them or
their semantics would be invalid / ambiguous without deeper protocol features:

Reserved now (assert if attempted in debug; ignored in release for streaming):

- `Date` – generated once per second and injected automatically.
- `Content-Length` – computed from the body (fixed) or set through `contentLength()` (streaming). Prevents
  inconsistencies between declared and actual size.
- `Connection` – determined by keep-alive policy (HTTP version, server config, request count, errors). User code
  supplying conflicting values could desynchronize connection reuse logic.
- `Transfer-Encoding` – controlled by streaming writer (`chunked`) or omitted when `Content-Length` is known. Allowing
  arbitrary values risks illegal CL + TE combinations or unsupported encodings.
- `Trailer`, `TE`, `Upgrade` – not yet supported by aeronet; reserving them now avoids future backward-incompatible
  behavior changes when trailer / upgrade features are introduced.

Allowed convenience helpers:

- `Content-Type` via `contentType()` or `setHeader("Content-Type", ...)` in streaming.
- `Location` via `location()` for redirects.

All other headers (custom application / caching / CORS / etc.) may be freely set; they are forwarded verbatim.
This central rule lives in a single helper (`http::IsReservedResponseHeader`).

## Miscellaneous features

### Connection Close Semantics (CloseMode)

Full details (modes, triggers, helpers) have been moved out of the landing page:
See: [Connection Close Semantics](docs/FEATURES.md#connection-close-semantics)

### Compression (gzip, deflate, zstd, brotli)

Detailed negotiation rules, thresholds, opt-outs, and tuning have moved:
See: [Compression & Negotiation](docs/FEATURES.md#compression--negotiation)

Per-response manual override: setting any `Content-Encoding` (even `identity`) disables automatic compression for that
response. Details & examples: [Manual Content-Encoding Override](docs/FEATURES.md#per-response-manual-content-encoding-automatic-compression-suppression)

### Inbound Request Body Decompression

Detailed multi-layer decoding behavior, safety limits, examples, and configuration moved here:
See: [Inbound Request Decompression](docs/FEATURES.md#inbound-request-decompression-config-details)

```cpp
server.router().setDefault([](const HttpRequest&, HttpResponseWriter& w){
  w.statusCode(200, "OK");
  w.contentType("text/plain");
  for (int i = 0;i < 10; ++i) {
    w.write(std::string(50,'x')); // accumulates until threshold then switches to compressed chunks
  }
  w.end();
});
```

### Request Header Duplicate Handling

Detailed policy & implementation moved to: [Request Header Duplicate Handling](docs/FEATURES.md#request-header-duplicate-handling-detailed)

### Inbound Request Body Decompression (Content-Encoding)

Detailed behavior, limits & examples moved to: [Inbound Request Decompression](docs/FEATURES.md#inbound-request-decompression-config-details)

## Test Coverage Matrix

Summary of current automated test coverage (see `tests/` directory). Legend: ✅ covered by explicit test(s), ⚠ partial / indirect, ❌ not yet.

| Area | Feature | Test Status | Notes / Test Files |
|------|---------|-------------|--------------------|
| Parsing | Request line (method/target/version) | ✅ | `http_basic.cpp`, malformed cases in `http_malformed.cpp` |
| Parsing | Unsupported HTTP version (505) | ✅ | `http_parser_errors.cpp` (InvalidVersion505) |
| Parsing | Header parsing & lookup | ✅ | Implicit in all request tests; malformed headers in `http_malformed.cpp` |
| Limits | Max header size -> 431 | ✅ | `http_malformed.cpp` (OversizedHeaders) |
| Limits | Max body size (Content-Length) -> 413 | ✅ | `http_additional.cpp` (ExplicitTooLarge413) |
| Limits | Chunk total/body growth -> 413 | ✅ | Chunk oversize paths indirectly via size guard (add targeted test later if needed) |
| Bodies | Content-Length body handling | ✅ | `http_basic.cpp`, others |
| Bodies | Chunked decoding | ✅ | `http_chunked_head.cpp`, fuzz in `http_parser_errors.cpp` |
| Bodies | Trailers exposure | ❌ | Not implemented |
| Expect | 100-continue w/ non-zero length | ✅ | `http_parser_errors.cpp` (Expect100OnlyWithBody) |
| Expect | No 100 for zero-length | ✅ | `http_parser_errors.cpp` (Expect100OnlyWithBody) & `http_additional.cpp` |
| Keep-Alive | Basic keep-alive persistence | ✅ | `http_keepalive.cpp` |
| Keep-Alive | Max requests per connection | ✅ | `http_additional.cpp` (CloseAfterLimit), `http_head_maxrequests.cpp` |
| Keep-Alive | Idle timeout close | ⚠ | Covered indirectly (tests rely on timely closure); no explicit timeout assertion |
| Pipelining | Sequential pipeline of requests | ✅ | `http_additional.cpp` (TwoRequestsBackToBack) |
| Pipelining | Malformed second request handling | ✅ | `http_additional.cpp` (SecondMalformedAfterSuccess) |
| Methods | HEAD semantics (no body) | ✅ | `http_chunked_head.cpp`, `http_head_maxrequests.cpp` |
| Date | RFC7231 format + correctness | ✅ | `http_date.cpp` (format + caching tests) |
| Date | Same-second caching invariance | ✅ | `http_date.cpp` (StableWithinSameSecond) |
| Date | Second-boundary refresh | ✅ | `http_date.cpp` (ChangesAcrossSecondBoundary) |
| Errors | 400 Bad Request (malformed line) | ✅ | `http_malformed.cpp` |
| Parsing | Percent-decoding of path | ✅ | `http_url_decoding.cpp` (spaces, UTF-8, '+', invalid percent -> 400) |
| Errors | 431, 413, 505, 501 | ✅ | Various tests (`http_malformed.cpp`, `http_additional.cpp`, version & TE tests) |
| Errors | PayloadTooLarge in chunk decoding | ⚠ | Path exercised via guard; add dedicated oversize chunk test future |
| Concurrency | SO_REUSEPORT distribution | ✅ | `http_multi_reuseport.cpp` |
| Lifecycle | Move semantics of server | ✅ | `http_server_move.cpp` |
| Lifecycle | Graceful stop (runUntil) | ✅ | All tests using runUntil pattern |
| Diagnostics | Parser error callback (version, bad line, limits) | ✅ | `http_parser_errors.cpp` |
| Diagnostics | PayloadTooLarge callback (Content-Length) | ⚠ | Indirect; could add explicit capture test |
| Performance | Date caching buffer size correctness | ✅ | Indirect via date tests + no crash (unit test ensures length) |
| Performance | writev header+body path | ⚠ | Indirect (no specific assertion) |
| Not Implemented | Trailers, outgoing chunked, compression, routing, TLS | ❌ | Roadmap |

Planned test additions (nice-to-have): oversize single chunk explicit test, keep-alive idle timeout explicit assertion, payload-too-large callback capture for chunk paths, writev behavior inspection (mock / interception).

## Build & Installation

Full, continually updated build, install, and package manager instructions live in [`docs/INSTALL.md`](docs/INSTALL.md).

Quick start (release build of examples):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

For TLS toggles, sanitizers, Conan/vcpkg usage and `find_package` examples, see the INSTALL guide.

## Trailing Slash Policy

Full resolution algorithm and matrix moved to: [Trailing Slash Policy](docs/FEATURES.md#trailing-slash-policy)

## Construction Model (RAII) & Ephemeral Ports

Overview relocated to: [Construction Model (RAII & Ephemeral Ports)](docs/FEATURES.md#construction-model-raii--ephemeral-ports)

## TLS Features (Current)

See: [TLS Features](docs/FEATURES.md#tls-features)

### TLS Metrics Reference

Metrics example: [TLS Features](docs/FEATURES.md#tls-features)

## OpenTelemetry Support (Experimental)

Aeronet provides optional OpenTelemetry integration for distributed tracing and metrics. Enable with the CMake flag `-DAERONET_ENABLE_OPENTELEMETRY=ON`. Be aware that it pulls also `protobuf` dependencies.

### Architecture

**Instance-based telemetry:** Each `HttpServer` maintains its own `TelemetryContext` instance. There are no global singletons or static state. This design:

- Allows multiple independent servers with different telemetry configurations
- Eliminates race conditions and global state issues
- Makes testing and multi-server scenarios straightforward
- Ties telemetry lifecycle directly to server lifecycle

All telemetry operations log errors via `log::error()` for debuggability—no silent failures.

### Dependencies

When OpenTelemetry is enabled, aeronet requires the following system packages:

**Debian/Ubuntu:**

```bash
sudo apt-get install libcurl4-openssl-dev libprotobuf-dev protobuf-compiler
```

**Alpine Linux:**

```bash
apk add curl-dev protobuf-dev protobuf-c-compiler
```

**Fedora/RHEL:**

```bash
sudo dnf install libcurl-devel protobuf-devel protobuf-compiler
```

**Arch Linux:**

```bash
sudo pacman -S curl protobuf
```

### Configuration

Configure OpenTelemetry via `HttpServerConfig`:

```cpp
#include <aeronet/aeronet.hpp>
using namespace aeronet;

int main() {
  HttpServerConfig cfg;
  cfg.withPort(8080)
     .withOtelConfig(OtelConfig{
       .enabled = true,
       .endpoint = "http://localhost:4318",  // OTLP HTTP endpoint
       .serviceName = "my-service",
       .sampleRate = 1.0  // 100% sampling for traces
     });
  
  HttpServer server(cfg);
  // Telemetry is automatically initialized when server.init() is called
  // Each server has its own independent TelemetryContext
  
  // ... register handlers ...
  server.run();
}
```

### Built-in Instrumentation

When OpenTelemetry is enabled, aeronet automatically tracks:

**Traces:**

- `http.request` spans for each HTTP request with attributes (method, path, status_code, etc.)

**Metrics:**

- `aeronet.events.processed` – epoll events successfully processed per iteration
- `aeronet.connections.accepted` – new connections accepted
- `aeronet.bytes.read` – bytes read from client connections
- `aeronet.bytes.written` – bytes written to client connections

All instrumentation happens automatically—no manual API calls required in handler code.

### Query String & Parameters

Details here: [Query String & Parameters](docs/FEATURES.md#query-string--parameters)

### Logging

Details moved to: [Logging](docs/FEATURES.md#logging)

## Streaming Responses (Chunked / Incremental)

Moved to: [Streaming Responses](docs/FEATURES.md#streaming-responses-chunked--incremental)

### Mixed Mode & Dispatch Precedence

Moved to: [Mixed Mode & Dispatch Precedence](docs/FEATURES.md#mixed-mode--dispatch-precedence)

## Configuration API (builder style)

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

HttpServer server(cfg); // or HttpServer(8080) then server.setConfig(cfgWithoutPort);
```

Keep-alive can be disabled globally by `cfg.withKeepAliveMode(false)`; per-request `Connection: close` or `Connection: keep-alive` headers are also honored (HTTP/1.1 default keep-alive, HTTP/1.0 requires explicit header).

### Handler Registration / Routing (Detailed)

Two mutually exclusive approaches:

1. Global handler: `server.router().setDefault([](const HttpRequest&){ ... })` (receives every request).
2. Per-path handlers: `server.router().setPath("/hello", http::Method::GET | http::Method::POST, handler)` – exact path match.

Rules:

- Mixing the two modes (calling `setPath` after `setDefault` or vice-versa) throws.
- If a path is not registered -> 404 Not Found.
- If path exists but method not allowed -> 405 Method Not Allowed.
- You can call `setPath` repeatedly on the same path to extend the allowed method mask (handler is replaced, methods merged).
- You can also call `setPath` once for several methods by using the `|` operator (for example: `http::Method::GET | http::Method::POST`)

Example:

```cpp
HttpServer server(cfg);
server.router().setPath("/hello", http::Method::GET | http::Method::PUT, [](const HttpRequest&){
  HttpResponse r; r.statusCode=200; r.reason="OK"; r.body="world"; r.contentType="text/plain"; return r; });
server.router().setPath("/echo", http::Method::POST, [](const HttpRequest& req){
  HttpResponse r; r.statusCode=200; r.reason="OK"; r.body=req.body; r.contentType="text/plain"; return r; });
// Add another method later (merges method mask, replaces handler)
server.router().setPath("/echo", http::Method::GET, [](const HttpRequest& req){
  HttpResponse r; r.statusCode=200; r.reason="OK"; r.body = "Echo via GET"; r.contentType="text/plain"; return r; });
```

### Limits

- 431 is returned if the header section exceeds `maxHeaderBytes`.
- 413 is returned if the declared `Content-Length` exceeds `maxBodyBytes`.
- Connections exceeding `maxOutboundBufferBytes` (buffered pending write bytes) are marked to close after flush (default 4MB) to prevent unbounded memory growth if peers stop reading.
- Slowloris protection: configure `withHeaderReadTimeout(ms)` to bound how long a client may take to send an entire request head (request line + headers). 0 disables.

### Performance / Metrics & Backpressure

`HttpServer::stats()` exposes aggregated counters:

- `totalBytesQueued` – bytes accepted into outbound buffering (including those sent immediately)
- `totalBytesWrittenImmediate` – bytes written synchronously on first attempt (no buffering)
- `totalBytesWrittenFlush` – bytes written during later flush cycles (EPOLLOUT)
- `deferredWriteEvents` – number of times EPOLLOUT was registered due to pending data
- `flushCycles` – number of flush attempts triggered by writable events
- `maxConnectionOutboundBuffer` – high-water mark of any single connection's buffered bytes

Use these to gauge backpressure behavior and tune `maxOutboundBufferBytes`. When a connection's pending buffer would exceed the configured maximum, it is marked for closure once existing data flushes, preventing unbounded memory growth under slow-reader scenarios.

### Metrics Callback (Scaffold)

You can install a lightweight per-request metrics callback capturing basic timing and size information:

```cpp
server.setMetricsCallback([](const HttpServer::RequestMetrics& m){
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

### Test HTTP Client Helper

The test suite uses a unified helper for simple GETs, streaming incremental reads, and multi-request keep-alive batches. See `docs/test-client-helper.md` for guidance when adding new tests.

### Roadmap additions

- [x] Connection write buffering / partial write handling
- [x] Outgoing chunked responses & streaming interface (phase 1)
- [ ] Trailing headers exposure for chunked requests
- [ ] Richer routing (wildcards, parameter extraction)
- [x] TLS (OpenSSL) support (basic HTTPS termination)
- [ ] Benchmarks & perf tuning notes

### TLS (HTTPS) Support

Details merged into: [TLS Features](docs/FEATURES.md#tls-features)

## Acknowledgements

Compression libraries (zlib, zstd, brotli), OpenSSL, Opentelemetry and spdlog provide the optional feature foundation; thanks to their maintainers & contributors.

## License

Licensed under the MIT License. See [LICENSE](LICENSE).
