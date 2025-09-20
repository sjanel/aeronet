# Aeronet

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
- [ ] 415 Unsupported Media Type (content-type based) – not required yet
- [ ] 405 Method Not Allowed (no method allow list presently)

Headers & protocol niceties

- [x] Date header (cached once per second)
- [x] Connection keep-alive / close
- [x] Content-Type (user supplied only)
- [x] Expect: 100-continue handling
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
