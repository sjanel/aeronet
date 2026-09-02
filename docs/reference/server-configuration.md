# Server configuration

`HttpServerConfig` is the value object passed to `SingleHttpServer` or `MultiHttpServer`. Build it before construction and keep it in application-owned code; listener binding, thread count, and protocol availability are construction-time choices. Every `with...()` function returns the configuration object, so settings chain naturally.

```cpp
using namespace std::chrono_literals;

aeronet::HttpServerConfig config;
config.withPort(8080)
    .withKeepAliveTimeout(15s)
    .withHeaderReadTimeout(10s)
    .withBodyReadTimeout(30s)
    .withMaxHeaderBytes(16 * 1024)
    .withMaxBodyBytes(32ULL * 1024 * 1024)
    .withMaxOutboundBufferBytes(8ULL * 1024 * 1024)
    .withMaxZerocopyPendingBytes(8ULL * 1024 * 1024)
    .withTcpNoDelayMode(aeronet::TcpNoDelayMode::Auto);
```

`HttpServer` is also named `MultiHttpServer`. `SingleHttpServer` always has one event loop and rejects `nbThreads > 1`; a multi-server uses `SO_REUSEPORT` internally when it starts more than one worker. Use `port = 0` during tests or when the OS should choose a port, then read `server.port()` after construction.

## Listener, lifecycle, and parser limits

| Setting | Default | Purpose |
| --- | --- | --- |
| `nbThreads` / `withNbThreads()` | 0 | Multi-server worker count; 0 selects hardware concurrency (one for `SingleHttpServer`). |
| `port` / `withPort()` | 0 | TCP listener port; 0 selects an ephemeral port. |
| `reusePort` / `withReusePort()` | false | Request `SO_REUSEPORT` for independent listeners. |
| `tcpNoDelay` / `withTcpNoDelay[Mode]()` | Auto | Nagle policy. Auto is the normal low-latency choice. |
| `enableKeepAlive` / `withKeepAliveMode()` | true | Allow HTTP/1.1 persistent connections. |
| `keepAliveTimeout` / `withKeepAliveTimeout()` | 5 s | Idle time after a response before a persistent connection is closed. |
| `maxRequestsPerConnection` | 100,000 | Bound requests served on a persistent connection. |
| `maxCachedConnections` | 10 | Reusable closed `ConnectionState` objects retained to reduce allocations. |
| `maxAcceptBatchSize` / `withMaxAcceptBatchSize()` | 64 | New accepts handled per event-loop iteration; `0` is unlimited. |
| `maxHeaderBytes` / `withMaxHeaderBytes()` | 8 KiB | Maximum request line, headers, and terminating CRLFCRLF. |
| `maxBodyBytes` / `withMaxBodyBytes()` | 256 MiB | Decoded request body ceiling. |
| `headerReadTimeout` / `withHeaderReadTimeout()` | disabled | Whole request-head deadline after the first byte; Slowloris protection. |
| `bodyReadTimeout` / `withBodyReadTimeout()` | disabled | Progress-based deadline while a handler waits for body data. |
| `mergeUnknownRequestHeaders` / `withMergeUnknownRequestHeaders()` | true | Comma-merge repeated unknown headers; use false for stricter custom-header semantics. |
| `traceMethodPolicy` / `withTracePolicy()` | Disabled | `Disabled`, `EnabledPlainAndTLS`, or `EnabledPlainOnly`. |
| `addTrailerHeader` / `withTrailerHeader()` | true | Add HTTP/1.1 `Trailer` listing for buffered responses with trailers. Streaming response headers are already sent before trailer names are known. |

## Event-loop, buffering, and performance controls

| Setting | Default | Purpose |
| --- | --- | --- |
| `maxOutboundBufferBytes` / `withMaxOutboundBufferBytes()` | 4 MiB | Per-connection retained-output cap. HTTP/1.1 drains then closes on overflow; HTTP/2 pauses wire-input processing at the queue high-water mark and rejects work that cannot fit its queued plus flow-control-deferred budget. |
| `maxZerocopyPendingBytes` / `withMaxZerocopyPendingBytes()` | 4 MiB | Per-connection payload cap for delayed Linux `MSG_ZEROCOPY` completions. New sends fall back to copied writes at the limit. |
| `minCapturedBodySize` / `withMinCapturedBodySize()` | 1 KiB | Fixed response bodies below this size are kept with the response head for locality. |
| `pollInterval` / `withPollInterval()` | 500 ms | Maximum idle event-loop wait. Lower values improve stop responsiveness but increase wakeups. |
| `pollIntervalMinFactor`, `pollIntervalMaxFactor` / `withPollIntervalFactors()` | 1.0, 1.0 | Adaptive poll bounds relative to `pollInterval`; `0.0, 2.0` permits a saturated spin and idle backoff. |
| `minReadChunkBytes` / `withMinReadChunkBytes()` | 4 KiB | Minimum follow-up inbound read size. |
| `maxPerEventReadBytes` / `withMaxPerEventReadBytes()` | 128 KiB | Fairness budget per connection event. Raise for few large uploads; lower for latency across many connections. |
| `zerocopyMode` / `withZerocopyMode()` | Opportunistic | `MSG_ZEROCOPY` policy for eligible large writes; Linux-specific. |
| `zerocopyMinBytes` / `withZerocopyMinBytes()` | 128 KiB | Minimum payload size for zero-copy attempts. |
| `globalHeaders` / `withGlobalHeaders()` / `addGlobalHeader()` | `server: aeronet` | Headers added unless the response already supplies that header. Maximum 256 entries. |

## HTTPS redirects and CONNECT

`withHttpsRedirect(port, status)` makes a plaintext listener redirect every request to the equivalent `https://` URL. `port = 0` disables it; 443 is omitted from the generated URL. Allowed statuses are 301, 302, 307, and 308. This is deliberately incompatible with TLS on the same listener.

```cpp
aeronet::HttpServerConfig redirect;
redirect.withPort(80).withHttpsRedirect(443, aeronet::http::StatusCodePermanentRedirect);

aeronet::HttpServerConfig https;
https.withPort(443).withTlsCertKey("/run/tls/tls.crt", "/run/tls/tls.key");
```

HTTP CONNECT tunnelling is fail-closed: its allowlist is empty by default. Set exact hostnames/IP strings with `withConnectAllowlist(first, last)`. The special single entry `"*"` grants access to all hosts and ports and should be reserved for a deliberately access-controlled proxy.

## TLS configuration

TLS requires `AERONET_ENABLE_OPENSSL=ON`. The convenience server methods enable TLS as needed; advanced fields are on `config.tls`.

| `TLSConfig` setting | Default | Use |
| --- | --- | --- |
| `enabled` | false | Master switch, enabled by certificate/key convenience methods. |
| `withCertFile()` / `withKeyFile()` | empty | PEM certificate chain and private-key paths. |
| `withCertPem()` / `withKeyPem()` | empty | In-memory PEM credentials, used when the corresponding file setting is empty. |
| `withTlsMinVersion()` / `withTlsMaxVersion()` | TLS 1.2 / unset | Protocol bounds; accepted server strings are `TLS1.2` and `TLS1.3`. TLS 1.2 is enforced when the minimum is not explicitly set. |
| `withCipherList()` / `withTlsCipherPolicy()` | empty / Default | OpenSSL TLS 1.2-and-earlier ciphers or a `Default`, `Modern`, `Compatibility`, or `Legacy` policy. |
| `withTlsAlpnProtocols()` | empty | Ordered ALPN preference list. Use `{ "h2", "http/1.1" }` for TLS HTTP/2. |
| `alpnMustMatch` | false | Fail a handshake with no ALPN overlap. |
| `requestClientCert`, `requireClientCert` | false | Request a certificate, or require and verify one for strict mTLS. Requiring implies requesting. |
| `withTlsTrustedClientCert()` | empty | Add a PEM trust anchor/leaf for client-certificate validation. |
| `handshakeTimeout` | disabled | Deadline from accept to TLS handshake completion. |
| `maxConcurrentHandshakes` | 0 | Concurrent handshake cap; 0 is unlimited. |
| `withTlsHandshakeRateLimit()` | 0, 0 | Per-second handshake limit and burst cap. |
| `logHandshake` | false | Log negotiated ALPN, cipher, version, and peer subject. |
| `disableCompression` | true | Keep TLS-level compression off to mitigate CRIME. |
| `ktlsMode` | Opportunistic | `Disabled`, `Opportunistic`, `Enabled` (warn if inactive), or `Required`; Linux/OpenSSL capability dependent. |
| `sessionTickets` | disabled, 2 keys, 24 h | Enable ticket rotation, choose lifetime/key slots, or supply static 48-byte keys. |
| `withTlsSniCertificateFiles/Memory()` | none | Add exact or wildcard SNI certificate mappings. |
| `withTlsOcspStapleFile()` | none | Load one successful DER OCSP response into the context cache for the default certificate. No handshake-time fetch is performed. |
| `TLSConfig::withTlsSniOcspStapleFile()` | none | Associate a DER OCSP response with an existing exact or wildcard SNI certificate mapping. |
| `withTlsCrlFile(path, checkAll)` | none, false | Load a PEM/DER CRL for inbound client-certificate verification; `checkAll` checks the full verified chain. |
| `withTlsRevocationCallback()` | none | Apply an application revocation decision after normal verification for each inbound client certificate. |
| `withTlsKeyLogFile()` | none | Append NSS-compatible traffic secrets in debug builds only. Release builds reject this setting. |

CRL and callback settings require `requestClientCert` or `requireClientCert`. OCSP inputs are passive operator-managed caches: refresh them with `postConfigUpdate()` before expiry. See [TLS and HTTP/2](../protocols/tls-and-http2.md) for the full lifecycle and security model and [production patterns](../guides/production-configuration.md) for a hardened listener profile.

## HTTP/2 configuration

HTTP/2 requires `AERONET_ENABLE_HTTP2=ON`; `Http2Config::enable` defaults to true. TLS uses ALPN, while cleartext can accept prior-knowledge h2c and HTTP/1.1 Upgrade when their options are enabled.

| Setting | Default | Meaning |
| --- | --- | --- |
| `enable`, `enablePush` | true, false | Global HTTP/2 switch and server push advertisement. |
| `enableH2c`, `enableH2cUpgrade` | true, true | Prior-knowledge and Upgrade cleartext HTTP/2 paths. |
| `enablePriority`, `maxPriorityTreeDepth` | true, 256 | PRIORITY processing and dependency-tree resource cap. |
| `mergeUnknownRequestHeaders` | true | Comma-merge unknown duplicate headers for HTTP/2. |
| `headerTableSize` | 4096 | `SETTINGS_HEADER_TABLE_SIZE`. |
| `maxConcurrentStreams` | 100 | `SETTINGS_MAX_CONCURRENT_STREAMS`. |
| `initialWindowSize` | 65,535 | Per-stream receive window. |
| `connectionWindowSize` | 1 MiB | Connection-level receive window. |
| `maxFrameSize` | 16,384 | Frame payload limit; valid range 16,384 through 16,777,215. |
| `maxHeaderListSize` | 8192 | Advertised decoded header-list ceiling. |
| `settingsTimeout` | 5 s | Deadline for the peer SETTINGS acknowledgement. |
| `pingInterval`, `pingTimeout` | disabled, 10 s | Optional PING keepalive and response deadline. |
| `maxStreamsPerConnection` | 1000000 | Lifetime stream count before graceful GOAWAY. |
| `maxStreamPendingBytes` | 4 MiB | Per-stream cap for in-memory response body and trailer bytes waiting on peer flow-control credit. Oversized fixed responses receive 503; overflowing streaming responses are reset with `ENHANCE_YOUR_CALM`. |

## Compression, decompression, and files

| Configuration | Defaults and decisions |
| --- | --- |
| `CompressionConfig` | `minBytes = 1024`, `maxCompressRatio = 0.6`, 32 KiB initial buffer, `Vary: Accept-Encoding` enabled, and automatic direct-compression mode. Choose `preferredFormats`, per-codec levels, and a content-type allowlist when CPU use needs controlling. Only compiled codecs can be negotiated. |
| `DecompressionConfig` | disabled for incoming requests by default. Once enabled, it accepts enabled/compiled codings and enforces the configured compressed-byte cap (`0` is unlimited), 4 GiB decoded cap, 32 KiB decoder chunks, 16 MiB streaming threshold, and optional expansion-ratio cap (`0` disables it). |
| `StaticFileConfig` | The file handler defaults to ranges and conditional requests enabled, 16 ranges, a 32 MiB assembled multipart-range cap, 1024 header-cache entries, strong ETags and Last-Modified, 128 KiB inline files, and no directory index/dotfiles. It also accepts custom MIME and directory-renderer callbacks. |

The static-file settings are supplied when creating the static handler, not directly on `HttpServerConfig`. Read [Bodies, streaming, and static files](../guides/bodies-streaming-and-files.md) for behavior and examples.

## Probes, logging, telemetry, and router policy

| Configuration | Defaults and behavior |
| --- | --- |
| `BuiltinProbesConfig` | Disabled; paths `/livez`, `/readyz`, `/startupz`. `dedicatedPort = 0` serves probes on the app listener. A nonzero dedicated port starts an isolated probe loop for `MultiHttpServer`; `livenessStaleThreshold` defaults to 10 seconds and must be positive then. |
| `AccessLogConfig` | Sink `None`, CLF format, no forwarded-for trust. Select `Stdout` or `File`, set `filePath` for the file sink, and set `useForwardedFor` only behind a trusted proxy that sanitizes the header. |
| `TelemetryConfig` | OTLP and DogStatsD both disabled; sample rate 1.0, export interval 10 seconds, timeout 5 seconds. Set endpoint/service name, optional exporter headers, DogStatsD Unix socket/tags/namespace, and explicit finite increasing histogram buckets. OTLP needs the build feature; DogStatsD Unix sockets are Linux-only. |
| `RouterConfig` | Trailing slash policy is `Normalize`; set `Strict` or `Redirect` as required. A route-level CORS policy wins over `withDefaultCorsPolicy()`. |

For serializable properties and their JSON/YAML names, build `aeronet-config-dump` with Glaze and use its emitted output as the baseline. See [Kubernetes](../kubernetes-examples.md) for a ConfigMap example.
