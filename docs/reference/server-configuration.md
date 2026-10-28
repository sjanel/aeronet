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
| `keepAliveTimeout` / `withKeepAliveTimeout()` | 5 s | Idle time after a response before a persistent connection is closed. WebSocket connections have their own [idle timeout](../protocols/websocket.md#idle-connections). Also bounds the [resolution of a CONNECT target](../protocols/connect.md#name-resolution) (`504` beyond). |
| `maxRequestsPerConnection` | 100,000 | Bound requests served on a persistent connection. |
| `maxCachedConnections` | 10 | Reusable closed `ConnectionState` objects retained to reduce allocations. |
| `maxAcceptBatchSize` / `withMaxAcceptBatchSize()` | 64 | New accepts handled per event-loop iteration; `0` is unlimited. |
| `maxHeaderBytes` / `withMaxHeaderBytes()` | 8 KiB | Maximum request line, headers, and terminating CRLFCRLF. Also bounds each chunk-size line and the trailers of a chunked request body. |
| `maxBodyBytes` / `withMaxBodyBytes()` | 256 MiB | Decoded request body ceiling. |
| `headerReadTimeout` / `withHeaderReadTimeout()` | disabled | Deadline to receive a whole HTTP/1.1 request head: from the accept for the first request of a connection (TLS handshake included), from its first byte for the next ones. Slowloris protection; HTTP/2, WebSocket and tunnel traffic are not concerned. |
| `bodyReadTimeout` / `withBodyReadTimeout()` | disabled | Progress-based deadline while a handler waits for body data. |
| `lingeringCloseTimeout` / `withLingeringCloseTimeout()` | 5 s | Maximum duration of the [lingering close](../guides/server-lifecycle.md#lingering-close) of an HTTP/1.1 connection closed while its client may still be sending; `0` closes it right away. |
| `mergeUnknownRequestHeaders` / `withMergeUnknownRequestHeaders()` | true | Comma-merge repeated unknown headers; use false for stricter custom-header semantics. |
| `traceMethodPolicy` / `withTracePolicy()` | Disabled | `Disabled`, `EnabledPlainAndTLS`, or `EnabledPlainOnly`. |
| `addTrailerHeader` / `withTrailerHeader()` | true | Add HTTP/1.1 `Trailer` listing for buffered responses with trailers. Streaming response headers are already sent before trailer names are known. |

## Event-loop, buffering, and performance controls

| Setting | Default | Purpose |
| --- | --- | --- |
| `maxOutboundBufferBytes` / `withMaxOutboundBufferBytes()` | 4 MiB | Per-connection retained-output cap. HTTP/1.1 drains then closes on overflow; HTTP/2 pauses wire-input processing at the queue high-water mark and rejects work that cannot fit its queued plus flow-control-deferred budget. A [CONNECT tunnel](../protocols/connect.md#backpressure) stops reading one side while this many bytes wait for the other, and HTTP/2 [reads files](../protocols/http2.md#flow-control-and-memory) ahead of the socket up to half of it. |
| `maxZerocopyPendingBytes` / `withMaxZerocopyPendingBytes()` | 4 MiB | Response bytes a connection may retain until the completion of their Linux `MSG_ZEROCOPY` sends, each retained buffer counting for at least `zerocopyMinBytes`. Writes beyond it are copied until completions release retained buffers; a single larger response may use zerocopy when nothing is retained. See [MSG_ZEROCOPY](../operations/performance.md#msg_zerocopy). |
| `minCapturedBodySize` / `withMinCapturedBodySize()` | 1 KiB | Fixed response bodies below this size are kept with the response head for locality. |
| `pollInterval` / `withPollInterval()` | 500 ms | Maximum idle event-loop wait. Lower values improve stop responsiveness but increase wakeups. |
| `pollIntervalMinFactor`, `pollIntervalMaxFactor` / `withPollIntervalFactors()` | 1.0, 1.0 | Adaptive poll bounds relative to `pollInterval`; `0.0, 2.0` permits a saturated spin and idle backoff. |
| `minReadChunkBytes` / `withMinReadChunkBytes()` | 4 KiB | Initial and smallest read size of a connection: doubled after each read filling it (up to 256 KiB, `kMaxReadChunkBytes`), halved after one filling less than a quarter of it. |
| `maxPerEventReadBytes` / `withMaxPerEventReadBytes()` | 128 KiB | Fairness budget per connection event: bytes read plus bytes written while serving them. Raise for few large uploads or downloads; lower for latency across many connections. Each read is at most the read size of the connection (see `minReadChunkBytes`); `std::numeric_limits<uint32_t>::max()` approximates no limit. |
| `zerocopyMode` / `withZerocopyMode()` | Opportunistic | `MSG_ZEROCOPY` policy for the large response writes of plaintext HTTP/1.1 connections; Linux-specific. `Opportunistic` skips loopback peers and stops on a connection once the kernel reports copied sends; `Enabled` uses it for every connection. See [MSG_ZEROCOPY](../operations/performance.md#msg_zerocopy). |
| `zerocopyMinBytes` / `withZerocopyMinBytes()` | 128 KiB | Minimum write size for `MSG_ZEROCOPY` (cannot be `0`). |
| `globalHeaders` / `withGlobalHeaders()` / `addGlobalHeader()` | `server: aeronet` | Headers added unless the response already supplies that header. Maximum 256 entries. The [reserved headers](../guides/responses.md#reserved-headers) and `location` are rejected by `validate()`. |

## HTTPS redirects and CONNECT

`withHttpsRedirect(port, status)` makes a plaintext listener redirect every request to the equivalent `https://` URL. `port = 0` disables it; 443 is omitted from the generated URL. Allowed statuses are 301, 302, 307, and 308. This is deliberately incompatible with TLS on the same listener. See [HTTP to HTTPS redirect](../protocols/tls.md#http-to-https-redirect).

```cpp
aeronet::HttpServerConfig redirect;
redirect.withPort(80).withHttpsRedirect(443, aeronet::http::StatusCodePermanentRedirect);

aeronet::HttpServerConfig https;
https.withPort(443).withTlsCertKey("/run/tls/tls.crt", "/run/tls/tls.key");
```

HTTP CONNECT tunnelling is fail-closed: its allowlist is empty by default. Set exact hostnames/IP strings in the `connectAllowlist` field, or with `withConnectAllowlist(first, last)`. The special single entry `"*"` grants access to all hosts and ports and should be reserved for a deliberately access-controlled proxy. See [CONNECT tunneling](../protocols/connect.md).

## TLS configuration

TLS requires `AERONET_ENABLE_OPENSSL=ON`. The convenience server methods enable TLS as needed; advanced fields are on `config.tls`.

| `TLSConfig` setting | Default | Use |
| --- | --- | --- |
| `enabled` | false | Master switch, enabled by certificate/key convenience methods. |
| `withCertFile()` / `withKeyFile()` | empty | PEM certificate chain (the leaf certificate, then its intermediate certificates, all sent to clients) and private-key paths. |
| `withCertPem()` / `withKeyPem()` | empty | In-memory PEM credentials, same format. Exclusive with the file settings: `validate()` rejects a configuration setting both. Encrypted private keys are rejected. |
| `withTlsMinVersion()` / `withTlsMaxVersion()` | TLS 1.2 / unset | Protocol bounds; accepted server strings are `TLS1.2` and `TLS1.3`. TLS 1.2 is enforced when the minimum is not explicitly set. A maximum below the minimum is rejected. |
| `withCipherList()` / `withTlsCipherPolicy()` | empty / Default | OpenSSL TLS 1.2 ciphers or a `Default`, `Modern`, `Compatibility`, or `Legacy` policy. A cipher list requires the `Default` policy. |
| `withTlsAlpnProtocols()` | empty | Ordered ALPN preference list. Use `{ "h2", "http/1.1" }` for TLS HTTP/2; `"h2"` is rejected when HTTP/2 is disabled. |
| `alpnMustMatch` | false | Fail a handshake with no ALPN overlap, including a client offering no protocol at all. |
| `requestClientCert`, `requireClientCert` | false | Request a certificate, or require and verify one for strict mTLS. Requiring implies requesting. |
| `withTlsTrustedClientCert()` | empty | Add a PEM trust anchor/leaf (or a bundle of them) for client-certificate validation. Their names are sent in the certificate request. |
| `handshakeTimeout` | 10 s | Deadline from accept to TLS handshake completion; 0 disables it. |
| `maxConcurrentHandshakes` | 0 | Concurrent handshake cap; 0 is unlimited. |
| `withTlsHandshakeRateLimit()` | 0, 0 | Per-second handshake limit and burst cap. |
| `logHandshake` | false | Log negotiated ALPN, cipher, version, and peer subject. |
| `disableCompression` | true | Keep TLS-level compression off to mitigate CRIME. |
| `ktlsMode` | Opportunistic | `Disabled`, `Opportunistic`, `Enabled` (warn if inactive), or `Required`; Linux/OpenSSL capability dependent. |
| `sessionTickets` | disabled, 2 keys, 24 h | Enable ticket rotation, choose lifetime/key slots, or supply static 48-byte keys. Sessions only resume with tickets, accepted for their lifetime. |
| `withTlsSniCertificateFiles/Memory()` | none | Add exact or wildcard SNI certificate mappings. A pattern can only be mapped once. |
| `withTlsOcspStapleFile()` | none | Load one successful DER OCSP response into the context cache for the default certificate. No handshake-time fetch is performed. |
| `TLSConfig::withTlsSniOcspStapleFile()` | none | Associate a DER OCSP response with an existing exact or wildcard SNI certificate mapping. |
| `withTlsCrlFile(path, checkAll)` | none, false | Load a PEM/DER CRL for inbound client-certificate verification; `checkAll` checks the full verified chain. |
| `withTlsRevocationCallback()` | none | Apply an application revocation decision after normal verification for each inbound client certificate. |
| `withTlsKeyLogFile()` | none | Append NSS-compatible traffic secrets in debug builds only. Release builds reject this setting. |

CRL and callback settings require `requestClientCert` or `requireClientCert`. OCSP inputs are passive operator-managed caches: refresh them before expiry by replacing the file and posting a config update with `postConfigUpdate()`, which reloads the TLS files changed on disk even at an unchanged path. See [TLS](../protocols/tls.md) for the full lifecycle and security model and [production patterns](../guides/production-configuration.md) for a hardened listener profile.

## HTTP/2 configuration

HTTP/2 requires `AERONET_ENABLE_HTTP2=ON`; `Http2Config::enable` defaults to true. TLS uses ALPN, while cleartext accepts prior-knowledge h2c when `enableH2c` is set. `Upgrade: h2c` requests are always answered over HTTP/1.1 (RFC 9113 §3.1 deprecated that mechanism). See [HTTP/2](../protocols/http2.md) for the protocol behavior and tuning guidance.

| Setting | Default | Meaning |
| --- | --- | --- |
| `enable` | true | Global HTTP/2 switch. |
| `enablePush` | false | Deprecated, no effect: server push is not supported and `SETTINGS_ENABLE_PUSH` is always advertised as 0. Kept so existing code and configuration files still compile and load. |
| `enableH2c` | true | Accept prior-knowledge cleartext HTTP/2 (client sends the connection preface directly). |
| `enableH2cUpgrade` | false | Deprecated, no effect: kept so existing code and configuration files still compile and load. |
| `enablePriority`, `maxPriorityTreeDepth` | true, 256 | PRIORITY processing and dependency-tree resource cap. |
| `mergeUnknownRequestHeaders` | true | Comma-merge unknown duplicate headers for HTTP/2. |
| `headerTableSize` | 4096 | `SETTINGS_HEADER_TABLE_SIZE`, the limit of both HPACK dynamic tables of a connection: a dynamic table size update of the peer above it is a `COMPRESSION_ERROR`, and the encoder keeps at most the smaller of it and the value advertised by the peer. A value below 4096 applies to the decoder once the peer acknowledges the `SETTINGS`. At most 65536. |
| `maxConcurrentStreams` | 100 | `SETTINGS_MAX_CONCURRENT_STREAMS`. |
| `initialWindowSize` | 1 MiB | Per-stream receive window (`SETTINGS_INITIAL_WINDOW_SIZE`). The RFC 9113 default of 65,535 bytes caps a transfer on one stream to about 64 KiB per round trip. |
| `connectionWindowSize` | 1 MiB | Connection-level receive window. |
| `maxFrameSize` | 16,384 | Frame payload limit; valid range 16,384 through 16,777,215. |
| `maxHeaderListSize` | 8192 | Decoded header-list ceiling, advertised to the peer and enforced: a larger header block resets its stream with `ENHANCE_YOUR_CALM`, and its fields beyond the limit are not stored while decoded. |
| `settingsTimeout` | 5 s | Deadline for the acknowledgement of the server `SETTINGS`, after which the connection is closed with `SETTINGS_TIMEOUT`; 0 disables it. |
| `pingInterval`, `pingTimeout` | disabled, 10 s | Optional PING keepalive: a connection with open streams but no activity for `pingInterval` is pinged, and closed without acknowledgement within `pingTimeout`. Idle connections are not pinged. |
| `maxStreamsPerConnection` | 1000000 | Streams of a connection: the last one sends a graceful `GOAWAY`, and the connection is closed once its streams are answered. Also the stream budget of an `HttpClient` connection. |
| `sendContentLengthHeader` | true | Add `content-length` to responses built with `HttpRequestView::makeResponse()`. Optional in HTTP/2; disabling it avoids moving the body when its length changes digit count. |
| `maxStreamPendingBytes` | 4 MiB | Per-stream cap for in-memory response body and trailer bytes waiting on peer flow-control credit. Oversized fixed responses receive 503; overflowing streaming responses are reset with `ENHANCE_YOUR_CALM`. |
| `maxRecycledRequests` | 8 | Server only, at most 255: request objects a connection keeps from its finished streams, reused by the next ones with the capacity of their header maps and buffers. Only connections receiving that many request bodies (or running that many suspended async handlers) at once need more; each one kept costs idle memory. 0 disables the recycling. |

## Compression, decompression, and files

| Configuration | Defaults and decisions |
| --- | --- |
| `CompressionConfig` | `minBytes = 1024`, `maxCompressRatio = 0.6`, 32 KiB initial buffer, `Vary: Accept-Encoding` enabled, and automatic direct-compression mode. Choose `preferredFormats`, per-codec levels, and a content-type allowlist when CPU use needs controlling. Only compiled codecs can be negotiated. See [Compression](../guides/compression.md). |
| `DecompressionConfig` | Enabled by default when a codec is compiled in; `enable = false` passes encoded bodies to handlers unchanged. Decodes the compiled codings with a 128 MiB compressed cap, a 4 GiB decoded cap, a 1000x expansion-ratio cap (both caps must be positive), 32 KiB decoder chunks, and a 16 MiB streaming threshold. See [Compression](../guides/compression.md#request-body-decompression). |
| `StaticFileConfig` | The file handler defaults to ranges and conditional requests enabled, 16 ranges, a 32 MiB assembled multipart-range cap, 1024 header-cache entries, strong ETags and Last-Modified, 128 KiB inline files, and no directory index/dotfiles. It also accepts custom MIME and directory-renderer callbacks. |

The static-file settings are supplied when creating the static handler, not directly on `HttpServerConfig`. Read [Static files](../guides/static-files.md#configuration) for behavior, examples, and every setting.

## Probes, logging, telemetry, and router policy

| Configuration | Defaults and behavior |
| --- | --- |
| `BuiltinProbesConfig` ([guide](../operations/health-probes.md)) | Disabled; paths `/livez`, `/readyz`, `/startupz`. `dedicatedPort = 0` serves probes on the app listener. A nonzero dedicated port starts an isolated probe loop for `MultiHttpServer`; `livenessStaleThreshold` defaults to 10 seconds and must be positive then. |
| `AccessLogConfig` | Sink `None`, CLF format, no forwarded-for trust. Select `Stdout` or `File`, set `filePath` for the file sink, and set `useForwardedFor` only behind a trusted proxy that sanitizes the header. |
| `TelemetryConfig` | OTLP and DogStatsD both disabled; sample rate 1.0, export interval 10 seconds, timeout 5 seconds. Set endpoint/service name, optional exporter headers, DogStatsD Unix socket/tags/namespace, and explicit finite increasing histogram buckets. OTLP needs the build feature; DogStatsD Unix sockets are Linux-only. |
| `RouterConfig` | Trailing slash policy is `Normalize`; set `Strict` or `Redirect` as required. A route-level CORS policy wins over `withDefaultCorsPolicy()`. |

For serializable properties and their JSON/YAML names, build `aeronet-config-dump` with Glaze and use its emitted output as the baseline. See [Kubernetes](../operations/kubernetes.md) for a ConfigMap example.
