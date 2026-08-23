# Client configuration

`HttpClientConfig` controls synchronous `HttpClient` behavior. It is available when `AERONET_ENABLE_HTTP_CLIENT=ON`; HTTPS additionally requires OpenSSL and HTTP/2 modes require `AERONET_ENABLE_HTTP2=ON`.

```cpp
using namespace std::chrono_literals;

aeronet::HttpClientConfig config;
config.connectTimeout = 5s;
config.requestTimeout = 20s;
config.withKeepAliveTimeout(25s)
    .withTcpNoDelay(true)
    .withCache(30s)
    .withCacheMaxEntries(256);
```

## Exchange, pool, and protocol controls

| Setting | Default | Behavior |
| --- | --- | --- |
| `connectTimeout` | 10 s | TCP connection deadline, including TLS handshake for HTTPS. |
| `requestTimeout` | 30 s | Deadline for one connected request/response exchange. |
| `maxResponseBytes` | 64 MiB | Maximum headers plus decoded body accepted before the client aborts. |
| `followRedirects`, `maxRedirects` | true, 5 | Follow redirects and bound the chain. |
| `keepAlive` | true | Reuse compatible HTTP connections. |
| `keepAliveTimeout` / `withKeepAliveTimeout()` | 30 s | Maximum age of an idle pooled connection; 0 disables expiry. |
| `maxIdleConnectionsPerHost` | 8 | Per-origin pool cap. |
| `httpVersion` / `withHttpVersion()` | Auto | `Auto` uses HTTPS ALPN when available and otherwise HTTP/1.1; `Http1_1` disables HTTP/2; `Http2` requires HTTP/2, including prior-knowledge h2c for plaintext. |
| `http2` / `withHttp2Config()` | `Http2Config` defaults | Native HTTP/2 settings and flow-control limits. Server-only settings such as h2c Upgrade, priority, and push do not affect a client. |
| `tcpNoDelay` / `withTcpNoDelay[Mode]()` | Auto | Request/response traffic normally benefits from Auto/Enabled TCP_NODELAY. |
| `globalHeaders` / `withGlobalHeaders()` / `addGlobalHeader()` | `user-agent: aeronet` | Headers supplied unless a request already provides a value. |
| `addTrailerHeader` | true | Add a `Trailer` header to requests that contain trailers. |
| `minCapturedBodySize` / `withMinCapturedBodySize()` | 1 KiB | Fold small captured HTTP/1.1 bodies into the request head buffer. |

## Compression and caching

The response decompressor is enabled by default whenever at least one codec is compiled in. If the caller does not set an explicit `Accept-Encoding`, it advertises the decoders actually compiled into the client. Decoded response bodies replace the compressed body and lose the `Content-Encoding` header. Set `withDefaultAcceptEncoding("identity")` to suppress this advertisement, or `withDecompression(false)` to stop automatic decoding.

`requestCompression` is distinct and opt-in. Call `withRequestCompression(Encoding::zstd)` or `withRequestCompression(true)` to encode outbound bodies that meet its `CompressionConfig::minBytes` and `maxCompressRatio` rules. The client leaves a body unchanged when the request already has `Content-Encoding` or `Transfer-Encoding`; selecting a codec that was not compiled in fails construction.

The built-in cache is also opt-in:

```cpp
using namespace std::chrono_literals;

aeronet::HttpClientConfig config;
config.withCache(15s)
    .withCacheMaxEntries(512)
    .withCacheMethods(aeronet::http::Method::GET | aeronet::http::Method::HEAD);
```

It stores only successful 2xx responses from the selected safe methods (GET, HEAD, and optionally OPTIONS). Entries are keyed by method, URL, headers, and body; they do not share different authorization headers. This is a TTL cache, not an HTTP-cache implementation: it deliberately does not interpret `Cache-Control`, ETags, `Vary`, `Age`, or revalidation. A zero TTL disables it, while `Duration::max()` never expires entries. The cache is owned by one `HttpClient` and follows its single-threaded assumption.

## Retries

`RetryConfig` has two layers. A safe stale pooled connection retry is always performed before sending the request and consumes no retry budget. Configured retries are disabled by default because `maxAttempts = 1`.

| `RetryConfig` setting | Default | Meaning |
| --- | --- | --- |
| `maxAttempts` | 1 | Total exchange attempts. Values below 1 behave as 1. |
| `baseDelay`, `multiplier`, `maxDelay` | 100 ms, 2.0, 2 s | Capped exponential backoff. |
| `jitter` | 0 | Random scale in `[1 - jitter, 1 + jitter]`; 0 is deterministic. |
| `retryStatuses` | 429, 503 | Responses eligible for a retry when attempts remain. Empty disables status retries. |
| `honorRetryAfter` | true | Respect delta-seconds `Retry-After`, capped by `maxDelay`; HTTP-date is not parsed. |
| `retryIdempotentAfterSend` | false | Permit retry after bytes were sent, only for idempotent methods. This is a deliberate resubmission decision. |

The client is synchronous, so backoff sleeps block the calling thread. Keep post-send retries off unless the operation is truly safe to repeat.

## Forward proxies and TLS

Use `withProxy("http://proxy.example:8080")` to send all traffic through a cleartext HTTP proxy. Plain HTTP uses absolute-form requests; HTTPS uses CONNECT first, then runs the origin TLS handshake through the tunnel. A bare `host[:port]` is accepted as an HTTP proxy with default port 80. HTTPS proxy URLs and malformed URLs are rejected. Pass a second argument to `withProxy()` for the CA file of an intercepting/debugging proxy.

With OpenSSL, the client verifies peer certificate chains and hostnames by default. Empty CA file/path settings use OpenSSL's system-default trust paths, including `SSL_CERT_FILE` and `SSL_CERT_DIR` where applicable.

| TLS setting | Default | Use |
| --- | --- | --- |
| `tlsVerifyPeer` | true | Verify chain and hostname for HTTPS. Do not disable outside controlled tests. |
| `withTlsCaFile()` / `withTlsCaPath()` | empty | Override trust bundle/file directory. |
| `withTlsMinVersion()` / `withTlsMaxVersion()` | TLS 1.2 / unset | Bound negotiated protocol version. |
| `withTlsCipherList()` | empty | OpenSSL cipher list for TLS 1.2 and below. |
| `withTlsClientCertKeyFile()` | empty | File-based client cert/key for mTLS. |
| `withTlsClientCertKeyMemory()` | empty | In-memory PEM client cert/key for mTLS. |

## Telemetry

`telemetry` is the same `TelemetryConfig` used by the server: set it directly and call `validate()` through client construction. OpenTelemetry must be compiled in for OTLP instrumentation. The [server configuration reference](server-configuration.md#probes-logging-telemetry-and-router-policy) lists its endpoint, sampling, DogStatsD, exporter-header, and histogram controls.
