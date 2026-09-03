# Production deployment patterns

These configurations are starting points, not universal presets. Set memory ceilings from the traffic and container budget you actually operate, and test any latency/performance tuning under representative load.

## Bounded public HTTP/1.1 listener

This profile places explicit limits around untrusted input and output, enables header/body deadlines, and keeps the event loop responsive to shutdown. It is appropriate as a baseline for a latency-sensitive public API.

```cpp
#include <aeronet/aeronet-server.hpp>

#include <array>
#include <chrono>

using namespace std::chrono_literals;

aeronet::HttpServerConfig config;
config.withPort(8080)
    .withKeepAliveTimeout(15s)
    .withHeaderReadTimeout(10s)
    .withBodyReadTimeout(30s)
    .withMaxHeaderBytes(16 * 1024)
    .withMaxBodyBytes(16U * 1024 * 1024)
    .withMaxOutboundBufferBytes(2U * 1024 * 1024)
    .withMaxZerocopyPendingBytes(2U * 1024 * 1024)
    .withMaxRequestsPerConnection(10'000)
    .withMaxAcceptBatchSize(64)
    .withPollInterval(100ms)
    .withTcpNoDelay(true);
```

`maxOutboundBufferBytes` and `maxZerocopyPendingBytes` are per connection: budget them alongside connection count. For HTTP/2, also set `http2.maxStreamPendingBytes` no higher than the response memory one stream may retain while waiting for flow-control credit. Set `maxPerEventReadBytes` lower if many clients need fairness or higher only when a small number of large request bodies should maximize throughput.

## TLS and HTTP/2 edge listener

Configure TLS with a certificate/key and explicitly order ALPN protocols. This preserves HTTP/1.1 for older clients while preferring HTTP/2. Disable h2c on a public TLS-only port when cleartext HTTP/2 is not part of the deployment.

```cpp
#include <aeronet/aeronet-server.hpp>

#include <chrono>

using namespace std::chrono_literals;

aeronet::Http2Config http2;
http2.withMaxConcurrentStreams(128)
    .withMaxStreamPendingBytes(2U << 20U)
    .withConnectionWindowSize(1U << 20U)
    .withPingInterval(30s)
    .withPingTimeout(10s)
    .withEnableH2c(false)
    .withEnableH2cUpgrade(false);

aeronet::HttpServerConfig config;
config.withPort(443)
    .withTlsCertKey("/run/secrets/tls.crt", "/run/secrets/tls.key")
    .withTlsAlpnProtocols({"h2", "http/1.1"})
    .withTlsHandshakeTimeout(10s)
    .withHttp2(http2);
config.tls.withTlsMinVersion("TLS1.2");
```

This needs `AERONET_ENABLE_OPENSSL=ON` and `AERONET_ENABLE_HTTP2=ON`. Add client-certificate trust material before enabling `withTlsRequireClientCert()`; use SNI mappings when multiple hostnames require separate certificates. `KtlsMode::Required` is only appropriate after verifying kernel, OpenSSL, and cipher compatibility in the target environment.

For certificates that publish an OCSP responder, fetch and verify the response out of process and load its DER file with `withTlsOcspStapleFile()`. Schedule refresh before `nextUpdate`, then use `postConfigUpdate()` so the new response is parsed and atomically installed for new connections. This keeps responder latency and outages out of the handshake path. If the listener verifies client certificates, add `withTlsCrlFile()` or a bounded, non-blocking `withTlsRevocationCallback()` according to your CA policy. The callback runs on the handshake path.

Never enable `withTlsKeyLogFile()` in a production build or image. It is rejected when `NDEBUG` is defined, and its contents allow decryption of captured sessions. See the [complete TLS guide](../protocols/tls-and-http2.md#tls) for OCSP ownership, CRL semantics, reload examples, and zeroization boundaries.

## HTTP to HTTPS redirect listener

Run the redirector and TLS listener separately. A redirect listener does not run application routes and cannot have TLS enabled.

```cpp
aeronet::HttpServerConfig http;
http.withPort(80).withHttpsRedirect(443, aeronet::http::StatusCodePermanentRedirect);

aeronet::HttpServerConfig https;
https.withPort(443).withTlsCertKey("/run/secrets/tls.crt", "/run/secrets/tls.key");
```

Use 308 where non-GET/HEAD methods must be retained across the redirect. The redirect URL keeps path and query and derives the host from the request Host header.

## Multi-worker server with isolated Kubernetes probes

For a multi-worker process, an isolated probe port remains answerable when every application worker is busy in a handler. Point Kubernetes probes to the dedicated port, not the application service port.

```cpp
#include <aeronet/aeronet-server.hpp>

#include <chrono>

using namespace std::chrono_literals;

aeronet::BuiltinProbesConfig probes;
probes.enabled = true;
probes.withDedicatedPort(8081)
    .withLivenessPath("/livez")
    .withReadinessPath("/readyz")
    .withStartupPath("/startupz")
    .withLivenessStaleThreshold(20s);

aeronet::HttpServerConfig config;
config.withPort(8080).withNbThreads(4).withBuiltinProbes(std::move(probes));
```

Dedicated probes apply to `MultiHttpServer`; a `SingleHttpServer` serves probes inline. The complete manifest and serializable config example are in [Kubernetes](../kubernetes-examples.md).

## Compression and upload decompression

Compression must be built with the corresponding codec. Use an allowlist to avoid burning CPU attempting to compress media that is normally already compressed, and apply decompression ceilings before accepting internet-facing uploads.

```cpp
aeronet::CompressionConfig compression;
compression.minBytes = 1024;
compression.maxCompressRatio = 0.7F;
compression.contentTypeAllowList.append("application/json");
compression.contentTypeAllowList.append("text/html");

aeronet::DecompressionConfig decompression;
decompression.enable = true;
decompression.maxCompressedBytes = 16ULL * 1024 * 1024;
decompression.maxDecompressedBytes = 64ULL * 1024 * 1024;
decompression.maxExpansionRatio = 20.0;

aeronet::HttpServerConfig config;
config.withCompression(std::move(compression)).withRequestDecompression(std::move(decompression));
```

An enabled decompressor still enforces each codec's build availability.

## Trusted proxy and CONNECT boundaries

Never broadly trust `Forwarded`/`X-Forwarded-For` just because an application happens to be behind a proxy. Set `accessLog.useForwardedFor` only where the proxy strips user-supplied values and is the sole client of the listener. CONNECT is disabled by default; an exact host allowlist is safer than `"*"`.

```cpp
const std::array<std::string_view, 2> allowedTargets{"updates.example.com", "api.example.net"};

aeronet::HttpServerConfig config;
config.withConnectAllowlist(allowedTargets.begin(), allowedTargets.end());
config.accessLog.sink = aeronet::AccessLogConfig::Sink::Stdout;
config.accessLog.format = aeronet::AccessLogConfig::Format::JSON;
config.accessLog.useForwardedFor = true;  // Only behind a sanitizing trusted proxy.
```

## Client for an upstream service

The client keeps its own pool, limits, optional retry policy, and optional TTL cache. Retries are blocking and a post-send retry is a resubmission, so keep that policy disabled unless the endpoint contract guarantees idempotency.

```cpp
#include <array>
#include <chrono>

using namespace std::chrono_literals;

aeronet::HttpClientConfig config;
config.connectTimeout = 3s;
config.requestTimeout = 10s;
config.maxResponseBytes = 8ULL * 1024 * 1024;
config.withKeepAliveTimeout(20s).withCache(5s).withCacheMaxEntries(256);

config.retry.maxAttempts = 3;
config.retry.baseDelay = 100ms;
config.retry.maxDelay = 1s;
```

See [client configuration](../reference/client-configuration.md) for proxy, mTLS, HTTP/2, request compression, and cache semantics.
