# Aeronet

![Aeronet Logo](resources/logo.png)

Experimental HTTP server library (Linux / epoll) – work in progress.

Features currently implemented:

- Epoll based edge-triggered event loop (one thread per HttpServer)
- Minimal zero-copy-ish HTTP/1.1 request parsing (request line + headers + Content-Length body)
- Chunked Transfer-Encoding (requests) decoding (no trailers exposed yet)
- Basic response builder
- Keep-Alive (with timeout + max requests per connection)
- Pipelined sequential request handling on a single connection
- Configurable limits: max header size, max body size
- Date header caching (1 update / second) to reduce formatting cost
- HEAD method support (suppresses body, preserves Content-Length)
- Expect: 100-continue handling
- Graceful shutdown via runUntil()
- Move semantics for HttpServer (transfer listening socket + loop)
- Multi-reactor horizontal scaling via SO_REUSEPORT
- Tests for basics, move semantics, reuseport distribution, keep-alive, header/body limits, chunked, HEAD, Expect

## HTTP/1.1 Feature Matrix

Legend: [x] implemented, [ ] planned / not yet.

Core parsing & connection handling

- [x] Request line parsing (method, target, version)
- [x] Header field parsing (no folding / continuations)
- [x] Case-insensitive header lookup helper
- [x] Persistent connections (HTTP/1.1 default, HTTP/1.0 opt-in)
- [x] HTTP/1.0 response version preserved (no silent upgrade)
- [x] Connection: close handling
- [x] Pipelined sequential requests (no parallel handler execution)
- [ ] Backpressure / partial write buffering

Request bodies

- [x] Content-Length bodies with size limit
- [x] Chunked Transfer-Encoding decoding (request) (ignores trailers)
- [ ] Trailer header exposure
- [ ] Multipart/form-data convenience utilities

Response generation

- [x] Basic fixed body responses
- [x] HEAD method (suppressed body, correct Content-Length)
- [ ] Outgoing chunked / streaming responses
- [ ] Compression (gzip / br)

Status & error handling

- [x] 400 Bad Request (parse errors, CL+TE conflict)
- [x] 413 Payload Too Large (body limit)
- [x] 431 Request Header Fields Too Large (header limit)
- [x] 501 Not Implemented (unsupported Transfer-Encoding)
- [x] 505 HTTP Version Not Supported
- [x] 400 on HTTP/1.0 requests carrying Transfer-Encoding
- [ ] 415 Unsupported Media Type (content-type based) – not required yet
- [ ] 405 Method Not Allowed (no method allow list presently)

Headers & protocol niceties

- [x] Date header (cached once per second)
- [x] Connection keep-alive / close
- [x] Content-Type (user supplied only)
- [x] Expect: 100-continue handling
- [x] Expect header ignored for HTTP/1.0 (no interim 100 sent)
- [ ] Server header (intentionally omitted to keep minimal)
- [ ] Access-Control-* (CORS) helpers

Performance / architecture

- [x] Single-thread event loop (one server instance)
- [x] Horizontal scaling via SO_REUSEPORT (multi-reactor)
- [x] writev scatter-gather for response header + body
- [ ] Benchmarks & profiling docs
- [ ] Zero-copy sendfile() support for static files

Safety / robustness

- [x] Configurable header/body limits
- [x] Graceful shutdown loop (runUntil)
- [ ] Slowloris style header timeout mitigation (per-connection read deadline)
- [ ] TLS termination (OpenSSL) – currently only linked, not enabled

Developer experience

- [x] Builder style ServerConfig
- [x] Simple lambda handler signature
- [ ] Routing / middleware helpers
- [ ] Pluggable logging interface

Misc

- [x] Move semantics for HttpServer
- [ ] Public API stability guarantee (pre-1.0)
- [ ] License file

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

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run example

```bash
./build/examples/aeronet-minimal 8080
```

Then visit <http://localhost:8080/>

### Multi-reactor (SO_REUSEPORT) example

You can start several independent event loops on the same port (kernel load balances incoming connections) by using
`enablePortReuse(true)` and running one `HttpServer` per thread:

```bash
./build/examples/aeronet-multi 8080 4   # port 8080, 4 threads
```

Each thread owns its own listening socket (SO_REUSEPORT) and epoll instance – no shared locks in the accept path.
This is the simplest horizontal scaling strategy before introducing a worker pool.

## Configuration API (builder style)

`ServerConfig` lives in `aeronet/server-config.hpp` and exposes fluent setters (withX naming):

```cpp
ServerConfig cfg;
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

### Limits

- 431 is returned if the header section exceeds `maxHeaderBytes`.
- 413 is returned if the declared `Content-Length` exceeds `maxBodyBytes`.

### Roadmap additions

- [ ] Connection write buffering / partial write handling
- [ ] Outgoing chunked responses & streaming interface
- [ ] Trailing headers exposure for chunked requests
- [ ] Simple routing helper
- [ ] TLS (OpenSSL) support
- [ ] Benchmarks & perf tuning notes
 
## License

TBD (will add an open-source license later).
