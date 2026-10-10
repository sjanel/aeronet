# HTTP client

`HttpClient` is a synchronous HTTP/1.1 and HTTP/2 client built on the same transport, TLS, compression, and HPACK layers as the server. It is meant for service-to-service calls, health checks, fetching key sets or configuration, and tests that exercise a live server.

The client is built when the `AERONET_ENABLE_HTTP_CLIENT` CMake option is `ON` (the default). HTTPS requires `AERONET_ENABLE_OPENSSL`, and HTTP/2 requires `AERONET_ENABLE_HTTP2`. Include `<aeronet/http-client.hpp>`, or the `<aeronet/aeronet-client.hpp>` umbrella header.

## A first request

```cpp
#include <aeronet/http-client.hpp>

#include <string_view>

HttpClient client;
const HttpClientResult result = client.get("https://example.com/health");
if (result) {
  const HttpResponse& response = *result;
  const auto status = response.status();
  const std::string_view contentType = response.headerValueOrEmpty(http::ContentType);
  const std::string_view body = response.bodyInMemory();  // de-chunked and decompressed
} else {
  const std::string_view reason = ErrcToStr(result.error());  // e.g. "operation timed out"
}
```

`get()`, `head()`, `post()`, `put()`, and `del()` cover the common cases. The [client example](../../examples/client-minimal.cpp) prints the status, headers, and body of any URL.

## Error model

Every request returns an `HttpClientResult`, a `std::expected<HttpResponse, HttpClientErrc>`. A request that completes holds the `HttpResponse`, **whatever its status**: a `404` or a `503` is a response, not an error. A request that cannot complete holds an `HttpClientErrc`, and never throws:

| `HttpClientErrc` | Cause |
| --- | --- |
| `invalidUrl` | Malformed or unsupported URL, or redirect `Location`. |
| `connectFailed` | DNS resolution or TCP connection failure, connection timeout included. |
| `tlsError` | TLS handshake failure, certificate verification included. |
| `timeout` | TLS handshake, request write, or response read not completed in time. |
| `writeError`, `ioError` | Transport write failure, internal I/O failure. |
| `connectionClosed` | Connection closed by the peer before a complete response. |
| `malformedResponse` | Response that cannot be parsed, or larger than `maxResponseBytes`. |
| `protocolUnsupported` | The server selected a protocol this build cannot speak, such as `h2` without HTTP/2 support. |
| `proxyError` | The forward proxy refused or failed to open the tunnel. |

`ErrcToStr()` returns a description for logs. Exceptions are reserved for setup errors: an invalid configuration, or a TLS setup failure (an unreadable CA file or client certificate) throws `HttpClientException` when the client is built or at its first HTTPS request, and `makeRequest()` throws `std::invalid_argument` for a malformed URL. An `https` URL in a build without OpenSSL throws `std::logic_error`.

## Requests

`makeRequest()` returns an `HttpRequest` pre-filled with the client's global headers (`user-agent: aeronet` by default), to complete before passing it to `request()`:

```cpp
#include <aeronet/http-client.hpp>

#include <utility>

HttpClient client;
auto request = client.makeRequest(http::Method::POST, "https://api.example.com/orders");
request.headerAddLine("x-request-id", "4f1c2a").body(R"({"item":"book","quantity":2})", "application/json");
const HttpClientResult created = client.request(std::move(request));
```

Header names are lower-case, which literals are checked for at compile time. The body can be set from a string view (copied), an owned `std::string` or `std::vector` (moved, without copy), a static buffer with `bodyStatic()` (referenced, without copy), JSON or YAML serialization with `bodyJson()` / `bodyYaml()` (see [JSON and configuration files](json-and-config-files.md)), or written in place with `bodyInlineSet()`. Trailers are added with `trailerAddLine()`.

A body can also come from an open file, which is streamed without being loaded in memory: with `sendfile()` on cleartext HTTP/1.1, by bounded reads encrypted on the fly over TLS, and as DATA frames under flow control over HTTP/2.

```cpp
#include <aeronet/file.hpp>
#include <aeronet/http-client.hpp>

#include <utility>

HttpClient client;
auto upload = client.makeRequest(http::Method::PUT, "https://storage.example.com/reports/2026-10.pdf");
upload.file(File("/var/reports/2026-10.pdf"), "application/pdf");  // or file(file, offset, length, type)
const HttpClientResult stored = client.request(std::move(upload));
```

## Responses

The response is an `HttpResponse`, the same type the server builds. `status()`, `headers()`, `headerValueOrEmpty()`, and `bodyInMemory()` read it. Header names are lower-case. The body is complete when the request returns: chunked transfer coding is removed, a compressed body is decoded (and its `Content-Encoding` header dropped), and `Content-Type` and `Content-Length` describe the decoded body. Other headers keep their received values.

The whole response is buffered, up to `maxResponseBytes` (64 MiB by default). The body takes over the client's receive buffer instead of being copied: chunked responses hand over their de-framing buffer, decompressed responses the decoder output, and HTTP/2 responses their DATA buffer. The client keeps an empty buffer of the same capacity for the next exchange.

## Connections

Connections are pooled per origin (scheme, host, and port) and reused while they stay idle less than `keepAliveTimeout` (30 seconds by default), up to `maxIdleConnectionsPerHost` (8) idle connections per origin. A pooled connection the server closed in the meantime is detected before the request is sent, and the request goes out on a new connection, which is always safe since nothing was transmitted.

The client has no background thread. Expired idle connections are closed when their origin is requested again, and by a sweep over all origins at most once per `keepAliveTimeout`, after a request. `clearIdleConnections()` closes them all, for instance before a long pause or after the servers were redeployed.

The request and response buffers keep the capacity of the largest exchange, so that later requests do not allocate. In a long-running client, call `releaseUnusedMemory()` periodically: it shrinks the buffers gradually, closes expired connections, and drops expired cache entries. See [Long-running clients](../reference/client-configuration.md#long-running-clients).

## HTTP/2

`HttpClientConfig::httpVersion` selects the protocol:

| `HttpVersionMode` | Behavior |
| --- | --- |
| `Auto` (default) | HTTPS negotiates `h2` through ALPN, with an `http/1.1` fallback. Cleartext HTTP uses HTTP/1.1. |
| `Http2` | Requires HTTP/2: ALPN `h2` only over HTTPS, prior knowledge over cleartext HTTP (h2c). |
| `Http1_1` | Never uses HTTP/2. |

A pooled HTTP/2 connection keeps its negotiated settings and HPACK tables across requests. The client is synchronous, so an HTTP/2 connection carries one stream at a time.

Before reusing a pooled HTTP/2 connection, the client processes the complete control frames received since the last request, without blocking. Harmless frames, such as PING, SETTINGS, or WINDOW_UPDATE, are handled and the connection is reused. GOAWAY, a closed connection, malformed or partial input make the client reconnect before sending any byte of the next request, so that a non-idempotent request is never sent twice.

## HTTPS

The client verifies the server certificate chain and host name by default. Without explicit CA file or directory, it trusts the system store: OpenSSL's default paths and the `SSL_CERT_FILE` and `SSL_CERT_DIR` environment variables, and, when neither variable is set, the usual system bundles such as `/etc/ssl/certs/ca-certificates.crt`. On Windows, it trusts the root certificates of the system certificate store. HTTPS thus works on minimal container images whose OpenSSL lacks its compiled-in CA directory.

```cpp
HttpClientConfig config;
config.withTlsCaFile("/etc/pki/internal-ca.pem")  // trust a private CA
    .withTlsClientCertKeyFile("/run/secrets/client.crt", "/run/secrets/client.key");  // mutual TLS
HttpClient client(config);
```

Version bounds, cipher lists, and in-memory client certificates are listed in the [client configuration reference](../reference/client-configuration.md#forward-proxies-and-tls). Disable `tlsVerifyPeer` only in tests.

## Redirects

Redirects (`301`, `302`, `303`, `307`, `308`) are followed by default, up to `maxRedirects` (5) per request; the last response is returned when the limit is reached, or when a `3xx` response has no `Location`. Following RFC 9110, a `303` turns the request into a body-less `GET`, as does a `301` or `302` answering a method other than `GET` or `HEAD`. `307` and `308` keep the method and body. Set `followRedirects = false` to receive the `3xx` responses.

!!! warning
    A redirect to another origin keeps every request header, `Authorization` and `Cookie` included, and a redirect from HTTPS to cleartext HTTP is followed. When a request carries credentials for a server you do not fully control, disable `followRedirects` and handle the redirects yourself.

## Retries

Beyond the free retry of a stale pooled connection, retries are disabled by default. `RetryConfig` enables them, with exponential backoff:

```cpp
#include <chrono>

RetryConfig retry;
retry.maxAttempts = 3;                       // 1 initial try + 2 retries
retry.baseDelay = std::chrono::milliseconds{200};
retry.jitter = 0.2F;                          // spread retries of a fleet of clients
// retry.retryStatuses defaults to 429 and 503; a delta-seconds Retry-After is honored

HttpClientConfig config;
config.withRetry(retry);
HttpClient client(config);
```

A connection failure, and a response status listed in `retryStatuses`, are retried. A failure after the request was sent is retried only for idempotent methods, and only when `retryIdempotentAfterSend` is set, because the server may have processed the first request. The backoff sleeps block the calling thread. The [client configuration reference](../reference/client-configuration.md#retries) details every setting.

## Compression

When a codec is compiled in (zlib, zstd, or brotli), the client advertises the codings it can decode in `Accept-Encoding` and decodes compressed responses transparently. `withDefaultAcceptEncoding("identity")` disables the advertisement, and `withDecompression(false)` the decoding.

Request compression is opt-in: `withRequestCompression(Encoding::zstd)` compresses request bodies that are large enough and compress well enough, using the same rules as the server's response compression. A request that already has a `Content-Encoding` is sent unchanged. See [Compression](../guides/compression.md) for the codecs and thresholds.

## Response cache

An optional time-based cache returns the stored response of an identical request (same method, URL, headers, and body) for a fixed duration:

```cpp
#include <chrono>

HttpClientConfig config;
config.withCache(std::chrono::seconds{30}).withCacheMaxEntries(256);
HttpClient client(config);
```

Only successful (`2xx`) responses to `GET` and `HEAD` are stored by default. This is deliberately not an HTTP cache: `Cache-Control`, `ETag`, `Vary`, and revalidation are ignored. Use it for data whose staleness you choose, such as a key set or a configuration document. `clearResponseCache()` empties it.

## Forward proxy

`withProxy("http://proxy.internal:3128")` sends every request through a cleartext HTTP proxy. A cleartext HTTP request is sent to the proxy with an absolute URL; an HTTPS request first opens a `CONNECT` tunnel to the origin, then performs the TLS handshake with the origin through it. The optional second argument is a CA file to trust an intercepting proxy that re-signs the origin certificates, such as mitmproxy. An HTTPS proxy URL, or a malformed one, throws `HttpClientException`; a proxy that refuses the tunnel yields `proxyError`.

## Threads and servers

An `HttpClient` is not thread-safe: it owns its event loop, connection pool, and buffers. Use one client per thread, or protect a shared one with a mutex. A client is cheap to keep: create it once per thread rather than per request, so that its connections are reused.

A request blocks its thread until it completes. Inside a server, never call the client from a synchronous handler, which would block the event loop and every connection it serves: call it from `deferWork()` in an [async handler](../guides/async-handlers.md#call-another-http-service).

## Telemetry

The client counts retries, redirects, and request compression outcomes (`aeronet.http_requests.*`) through the `TelemetryConfig` of its configuration, exported like the server's metrics (see [Observability](../operations/observability.md)).

## Limitations

- The client is synchronous: one exchange at a time per client, and one stream at a time per HTTP/2 connection. A coroutine API integrated with a server event loop is on the [roadmap](../ROADMAP.md).
- A running request cannot be cancelled; it ends with its timeouts.
- Responses are buffered entirely, up to `maxResponseBytes`: there is no streaming of response bodies.
- There is no cookie storage.
- Only cleartext HTTP proxies are supported, without proxy authentication.

## Tests

- HTTP/1.1 exchanges, pooling, redirects, retries, cache, and response buffers: [http-client-core_test.cpp](../../aeronet/client/test/http-client-core_test.cpp).
- HTTP/2: [http-client-http2-e2e_test.cpp](../../aeronet/client/test/http-client-http2-e2e_test.cpp).
- HTTPS, trust stores, and mutual TLS: [http-client-tls-e2e_test.cpp](../../aeronet/client/test/http-client-tls-e2e_test.cpp).
- Configuration and retry policy: [http-client-config_test.cpp](../../aeronet/client/test/http-client-config_test.cpp) and [retry-config_test.cpp](../../aeronet/client/test/retry-config_test.cpp).
- Response decompression and `Accept-Encoding`: [client-accept-encoding_test.cpp](../../aeronet/client/test/client-accept-encoding_test.cpp).
