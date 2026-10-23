# Testing

aeronet's tests use GoogleTest and run with CTest. Unit tests live next to their module, in `aeronet/<module>/test/`, and cross-module integration tests, which start real servers on loopback sockets, live in `tests/`. Shared helpers (test clients, temporary files, TLS certificates, fault injection) are in `aeronet/test_support/`.

```bash
cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Debug -D AERONET_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure --timeout 300
```

## Test coverage matrix

Summary of the automated test coverage; the files named are in [tests/](../../tests/) unless a module path is given. Legend: ✅ covered by explicit tests, ⚠ partial or indirect.

| Area | Feature | Status | Representative test files |
| --- | --- | --- | --- |
| Parsing | Request line (method, target, version), header parsing and lookup | ✅ | `http-core_test.cpp`, `aeronet/server/test/http-parser_test.cpp` |
| Parsing | Percent-decoding of the path | ✅ | `http-core_test.cpp`, `aeronet/tech/test/url-encode-decode_test.cpp` |
| Parsing | Unsupported HTTP version (505), unsupported method (501), malformed request line (400) | ✅ | `http-errors_test.cpp`, `http-core_test.cpp` |
| Limits | Maximum header size (431) | ✅ | `http-core_test.cpp`, `http-additional_test.cpp` |
| Limits | Maximum body size (413), `Content-Length` and chunked growth | ✅ | `http-additional_test.cpp`, `http-request-decompression_test.cpp` |
| Bodies | `Content-Length` bodies, chunked decoding | ✅ | `http-core_test.cpp`, `http-errors_test.cpp`, `aeronet/server/test/http-parser_test.cpp` |
| Bodies | Trailers (incoming and outgoing) | ✅ | `http-core_test.cpp`, `aeronet/http/test/http-response_test.cpp` |
| Expect | `100-continue` for a non-empty body, none for an empty one | ✅ | `http-errors_test.cpp`, `http-additional_test.cpp` |
| Keep-alive | Persistence, maximum requests per connection | ✅ | `http-core_test.cpp`, `http-server-lifecycle_test.cpp` |
| Keep-alive | Idle timeout close, active connections outliving the timeout | ✅ | `http-core_test.cpp` |
| Connection close | Lingering close after an early response or before pipelined requests (client close, reset, timeout, configuration update, TLS `close_notify`, shutdown failure) | ✅ | `http-core_test.cpp`, `http-tls-io_test.cpp`, `http-errors_test.cpp` |
| Pipelining | Sequential pipelines, malformed second request, bodies and split requests behind served ones, TRACE, CONNECT, `Expect`, async handlers and WebSocket upgrades (with coalesced frames) behind served requests, batched responses | ✅ | `http-core_test.cpp`, `http-errors_test.cpp`, `http-additional_test.cpp`, `http-connect_test.cpp`, `http-async_test.cpp`, `websocket-integration_test.cpp` |
| Methods | `HEAD` semantics (no body) | ✅ | `http-core_test.cpp`, `aeronet/http/test/router_test.cpp` |
| Date | RFC 9110 format, same-second caching, second-boundary refresh | ✅ | `aeronet/server/test/date-header-cache_test.cpp`, `http-core_test.cpp` |
| Diagnostics | Parser error callback (version, bad line, limits) | ✅ | `http-errors_test.cpp` |
| Concurrency | `SO_REUSEPORT` distribution | ✅ | `multi-http-server_test.cpp`, `http-server-lifecycle_test.cpp` |
| Lifecycle | Move and copy semantics, restart, graceful stop and drain, `start()` | ✅ | `http-server-lifecycle_test.cpp` |
| Performance | `writev` header and body path | ⚠ | Exercised indirectly (`http-errors_test.cpp`), no direct assertion |
| TLS | Handshake, rejection behavior, I/O, OCSP stapling | ✅ | `http-tls-handshake_test.cpp`, `http-tls-io_test.cpp`, `aeronet/tls/test/tls-components_test.cpp` |
| Streaming | Streaming responses and incremental flush | ✅ | `http-core_test.cpp` |
| Routing | Path and method matching | ✅ | `http-routing_test.cpp`, `aeronet/http/test/router_test.cpp` |
| Protocol parity | Request pipeline over HTTP/1.1 and h2c: middleware and its metrics, CORS, trailing slash and HTTPS redirects, handler errors, streaming and async handlers | ✅ | `http-protocol-parity_test.cpp` |
| Compression | Negotiation, outbound insertion, inbound decompression | ✅ | `http-compression_test.cpp`, `http-request-decompression_test.cpp` |
| OpenTelemetry | Integration and end-to-end export | ✅ | `opentelemetry-e2e_test.cpp`, `aeronet/objects/test/opentelemetry-integration_test.cpp` |
| Probes and statistics | Built-in probes, server statistics | ✅ | `http-server-lifecycle_test.cpp`, `http-core_test.cpp` |

## Protocol parity

HTTP/1.1 and HTTP/2 share the steps of the request pipeline that follow the parsing of a request (`RequestPipeline`, in `aeronet/http-request-dispatch.hpp`): the request middleware, the CORS origin check, the handler call, the fallback responses, and the response middleware. Each protocol still drives them from its own code, around its protocol-specific parts (HTTP/1.1 `Expect`, body framing, WebSocket upgrades; HTTP/2 streams and flow control). A behavior of the pipeline is tested once for both protocols in [tests/http-protocol-parity_test.cpp](../../tests/http-protocol-parity_test.cpp), whose tests run with an `HttpClient` speaking HTTP/1.1, then cleartext HTTP/2 with prior knowledge: add the scenario there when changing the pipeline.

## Network fault injection

Real networks deliver partial reads and writes, return `EAGAIN` at arbitrary points, and reset connections. Loopback tests rarely exercise these paths, so aeronet provides deterministic fault injection at the transport layer.

The hooks are compiled when `AERONET_ENABLE_TEST_HOOKS` is `ON`, which is the default for test builds that are not `Release`. They cost nothing when disabled.

| Component | Header | Role |
| --- | --- | --- |
| `FaultPolicy` | `aeronet/fault-policy.hpp` | Describes the faults: maximum bytes per read or write, periodic `EAGAIN` every N reads or writes, connection reset after a byte count or on the next operation, and an optional seed to randomize partial sizes. |
| `TestPipe` | `aeronet/test-pipe.hpp` | In-memory bidirectional byte channel, for tests without sockets or event loop. |
| `TestTransport` | `aeronet/test-transport.hpp` | In-memory `Transport` over a `TestPipe`, with a `FaultPolicy` applied, for unit tests of protocol handlers. |
| `FaultInjectingTransport` | `aeronet/fault-injecting-transport.hpp` | Decorator applying a `FaultPolicy` to a real transport, such as a plain socket. |
| `ScopedTransportDecorator` | `aeronet/transport-test-hook.hpp` | Installs a decorator that the server applies to every transport it accepts, for integration tests with real sockets and the full event loop. |

Unit tests of the components are in [test-transport_test.cpp](../../aeronet/sys/test/test-transport_test.cpp). Integration tests in [tests/network-fault-injection_test.cpp](../../tests/network-fault-injection_test.cpp) exercise partial reads and writes, simulated `EAGAIN`, connection resets, combined faults, TLS read-ahead, and pipelining under faults, against the full server.

Simulated `EAGAIN` on reads does not combine with edge-triggered `epoll` in integration tests: when the transport reports no data although the socket still holds some, no new edge arrives to wake the server. Use it in unit tests without event loop, or simulate `EAGAIN` on writes, which works in both.
