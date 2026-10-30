# HTTP client

`HttpClient` is a synchronous HTTP/1.1 and HTTP/2 client built on the same transport, TLS, compression, and HPACK layers as the server. It is meant for service-to-service calls, health checks, fetching key sets or configuration, and tests that exercise a live server.

The client is built when the `AERONET_ENABLE_HTTP_CLIENT` CMake option is `ON` (the default). HTTPS requires `AERONET_ENABLE_OPENSSL`, and HTTP/2 requires `AERONET_ENABLE_HTTP2`. Include `<aeronet/http-client.hpp>`, or the `<aeronet/aeronet-client.hpp>` umbrella header. Its `HttpClientConfig` is described in the [client configuration reference](../reference/client-configuration.md); with Glaze, it can be loaded from a [JSON or YAML file](json-and-config-files.md#client-configuration-files).

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
| `timeout` | TLS handshake, request write, or response read not completed in time (see [Timeouts](#timeouts)). |
| `writeError`, `ioError` | Transport write failure, internal I/O failure. |
| `connectionClosed` | Connection closed (or HTTP/2 stream reset) by the peer before a complete response, such as a body shorter than its `Content-Length`. |
| `malformedResponse` | Response that cannot be parsed, or body that cannot be decoded: corrupt or truncated content coding, unsupported `Content-Encoding`. Over HTTP/1.1, also a status line whose version is not `HTTP/1.x` (a higher minor version, such as `HTTP/1.2`, is processed as HTTP/1.1). Over HTTP/2, also a body longer or shorter than its `content-length`. |
| `responseTooLarge` | Response larger than `maxResponseBytes`, or whose decoding exceeds the [decompression limits](../guides/compression.md#request-body-decompression) (`maxCompressedBytes`, `maxDecompressedBytes`, `maxExpansionRatio` of `HttpClientConfig::decompression`). |
| `aborted` | Transfer stopped by the [`ResponseBodySink`](#streaming-response-bodies) of the request. |
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

Header names are lower-case, which literals are checked for at compile time. The body can be set from a string view (copied), an owned `std::string`, `std::vector` or `std::unique_ptr` buffer (moved, without copy), a static buffer with `bodyStatic()` (referenced, without copy), JSON or YAML serialization with `bodyJson()` / `bodyYaml()` (see [JSON and configuration files](json-and-config-files.md)), or written in place with `bodyInlineSet()`. `makeRequest(method, url, body, type)` sets the body directly, copied or moved the same way: `client.makeRequest(http::Method::POST, url, std::move(json), "application/json")` does not copy `json`. Trailers are added with `trailerAddLine()`.

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

The response is an `HttpResponse`, the same type the server builds. `status()`, `headers()`, `headerValueOrEmpty()`, and `bodyInMemory()` read it. Header names are lower-case. The body is complete when the request returns: chunked transfer coding is removed, a compressed body is decoded (and its `Content-Encoding` header dropped, also for an empty body), and `Content-Type` and `Content-Length` describe the decoded body. Other headers keep their received values, except the codings of a `Content-Encoding` kept (decompression disabled), which are lower-cased as they are case-insensitive.

The whole response is buffered, up to `maxResponseBytes` (64 MiB by default), unless its body is [streamed to a sink](#streaming-response-bodies). The body takes over the client's receive buffer instead of being copied: chunked responses hand over their de-framing buffer, decompressed responses the decoder output, and HTTP/2 responses their DATA buffer. The client keeps an empty buffer of the same capacity for the next exchange.

## Streaming response bodies

To download a body too large to hold in memory, or to process it while it arrives, give the request a `ResponseBodySink` (`<aeronet/response-body-sink.hpp>`, included by `<aeronet/http-client.hpp>`). The client then delivers the body piece by piece, de-chunked and decoded like a buffered body, and only keeps one read of it in memory whatever its size. The returned `HttpResponse` holds the status and headers only.

```cpp
#include <aeronet/http-client.hpp>

#include <chrono>
#include <cstdio>
#include <string_view>

// Writes the body of a 200 response to a file as it arrives.
class FileSink final : public ResponseBodySink {
 public:
  explicit FileSink(std::FILE* file) noexcept : _file(file) {}

  bool onHead(const HttpResponse& head, const ResponseBodyInfo& body) override {
    if (body.sizeKnown()) {
      std::printf("downloading %zu bytes of %.*s\n", body.size, static_cast<int>(body.contentType.size()),
                  body.contentType.data());
    }
    return head.status() == http::StatusCodeOK;  // otherwise stop: the request fails with aborted
  }

  bool onBody(std::string_view chunk) override {
    return std::fwrite(chunk.data(), 1, chunk.size(), _file) == chunk.size();  // stop on a write error
  }

 private:
  std::FILE* _file;
};

int main() {
  HttpClientConfig config;
  config.requestTimeout = std::chrono::hours{1};        // the whole download
  config.inactivityTimeout = std::chrono::seconds{30};  // but fail fast when it stalls
  HttpClient client(config);

  std::FILE* file = std::fopen("export.csv", "wb");
  if (file == nullptr) {
    return 1;
  }
  FileSink sink(file);
  const HttpClientResult result = client.get("https://exports.example.com/2026-10.csv", sink);
  std::fclose(file);
  return result ? 0 : 1;  // ErrcToStr(result.error()) tells why it failed
}
```

`request(request, sink)` does the same for any request. The sink sees one response per request, the final one:

- `onHead()` is called once, before any body byte, with the status and headers, and a `ResponseBodyInfo` describing the body. A decoded body has no `Content-Encoding` header anymore.
    - `contentType` is the media type of the body: its `Content-Type`, `application/octet-stream` when absent, empty when the head announces no body (`HEAD` request, `204`, `304`) or an empty one.
    - `size` is the exact number of bytes `onBody()` will deliver, when known in advance (`sizeKnown()`): the `Content-Length` of a body delivered as received, the size of a buffered body (see below), `0` for an empty body. It is `ResponseBodyInfo::kUnknownSize` for a chunked body, a body delimited by the closing of the connection, or one decoded as it arrives, whose `Content-Length` counts the coded bytes. A body shorter than announced fails (`connectionClosed` over HTTP/1.1, `malformedResponse` over HTTP/2), and a sink never receives more.
- `onBody()` receives the consecutive pieces of the body, never empty. A piece is only valid during the call. A body without content coding is delivered from the client's receive buffer, without copy: what each read brought, or, for a chunked body, each chunk (or the part of it received) of at least 1 KiB, the smaller chunks received in a row being gathered and delivered together.
- Either callback returns `false` to stop the transfer: the request then fails with `aborted`. Over HTTP/1.1, a connection stopped while receiving the body is closed. Over HTTP/2, only the stream is reset (`CANCEL`), and the connection stays pooled.
- The responses of followed redirects and of statuses retried by the [retry policy](#retries) are buffered and discarded as usual, never given to the sink. A body with stacked content codings (`Content-Encoding: gzip, br`) is buffered, decoded whole, then delivered in one piece, from the client's buffers (without copy). These buffered bodies are bounded by `maxResponseBytes` and the decompression limits as usual. A content coding the client cannot decode fails with `malformedResponse` before the body is received.
- Once `onHead()` was called, the request is never retried: a failure leaves the sink with a partial body. A body cut short fails with `connectionClosed`, a truncated or corrupt content coding with `malformedResponse`.
- For a streamed body, `maxResponseBytes` only bounds the head, and `maxCompressedBytes` does not apply. The decoded size is still bounded by `maxDecompressedBytes`, and its ratio to the coded size by `maxExpansionRatio` (`responseTooLarge`): while the body arrives, the ratio is checked against at least one `decoderChunkSize` of coded bytes, so that a very compressible start does not stop a body whose overall ratio is fine.
- The [response cache](#response-cache) is not used.

The callbacks run on the thread of the request, and must not use the `HttpClient` making it. An exception thrown by a callback propagates out of `request()`, and the connection is closed.

## Timeouts

`connectTimeout` (10 s) bounds the TCP connection, which then fails with `connectFailed`, and the TLS handshake. `requestTimeout` (30 s) bounds the whole exchange once connected, whatever its progress. `inactivityTimeout` (60 s, like the idle timeout of common load balancers and proxies) bounds each wait for the connection to send or receive a byte: combined with a large `requestTimeout`, a long download completes as long as data keeps flowing, while a stalled one fails fast. As the default `requestTimeout` is shorter, it only takes effect once `requestTimeout` is raised above it. It must be positive: to rely on `requestTimeout` alone, give both the same value. An expired TLS handshake, request or inactivity timeout ends the request with `timeout`.

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

A pooled HTTP/2 connection keeps its negotiated settings and HPACK tables across requests. The client is synchronous, so an HTTP/2 connection carries one stream at a time. A failure confined to the stream of a request (malformed or too large response, truncated content coding, stream reset by the server, transfer stopped by a [sink](#streaming-response-bodies)) resets that stream if needed (`PROTOCOL_ERROR` for a malformed response, `CANCEL` otherwise) and keeps the connection pooled: the request fails, but the next ones reuse the connection, and the frames still arriving on the reset stream are discarded.

Before reusing a pooled HTTP/2 connection, the client processes the frames received since the last request, without blocking. Harmless frames, such as PING, SETTINGS, WINDOW_UPDATE, or the DATA still arriving on a reset stream, are handled and the connection is reused, even when the last of them is only partly received. GOAWAY (even partly received), a closed connection, or malformed input make the client reconnect before sending any byte of the next request, so that a non-idempotent request is never sent twice.

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

Redirects (`301`, `302`, `303`, `307`, `308`) are followed by default, up to `maxRedirects` (5) per request; the last response is returned when the limit is reached, or when a `3xx` response has no `Location`. Following RFC 9110, a `303` turns the request into a body-less `GET`, as does a `301` or `302` answering a method other than `GET` or `HEAD`. `307` and `308` keep the method and body. A redirect that is not followed (invalid `Location`, downgrade to cleartext) leaves the request unchanged. Set `followRedirects = false` to receive the `3xx` responses.

A redirect never weakens the protection of the request:

- **Credentials stay with their origin.** When the redirect leads to another origin (scheme, host, or port), the `Authorization` and `Cookie` headers are removed from the request, including those coming from `globalHeaders`. Other headers are kept. A redirect within the same origin keeps every header.
- **No downgrade to cleartext.** A redirect from HTTPS to HTTP is not followed: the `3xx` response is returned, and a warning is logged. A redirect from HTTP to HTTPS is followed.

!!! note
    Only `Authorization` and `Cookie` are recognized as credentials. If an API key travels in another header (`X-Api-Key`...), disable `followRedirects` for requests that carry it and handle the redirects yourself.

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
- A running request cannot be cancelled from another thread; it ends with its timeouts, or when its `ResponseBodySink` stops it.
- Request bodies are sent from memory or from a file: there is no callback to produce them while they are sent.
- There is no cookie storage.
- Only cleartext HTTP proxies are supported, without proxy authentication.

## Tests

- HTTP/1.1 exchanges, pooling, redirects (and the credentials they drop), retries, cache, timeouts, response buffers, and response body sinks (also over HTTP/2 for content-coded bodies and the `content-length` check): [http-client-core_test.cpp](../../aeronet/client/test/http-client-core_test.cpp). Request building and redirect URL resolution: [http-request_test.cpp](../../aeronet/client/test/http-request_test.cpp).
- HTTP/1.1 response parsing, including body streaming and the failure statuses: [http1-response-parser_test.cpp](../../aeronet/client/test/http1-response-parser_test.cpp).
- HTTP/2, including response body sinks and the connection kept after a stream failure: [http-client-http2-e2e_test.cpp](../../aeronet/client/test/http-client-http2-e2e_test.cpp).
- HTTPS, trust stores, mutual TLS, and redirects to cleartext: [http-client-tls-e2e_test.cpp](../../aeronet/client/test/http-client-tls-e2e_test.cpp).
- Configuration and retry policy: [http-client-config_test.cpp](../../aeronet/client/test/http-client-config_test.cpp) and [retry-config_test.cpp](../../aeronet/client/test/retry-config_test.cpp).
- Configuration files: [client-config-loader_test.cpp](../../aeronet/client/test/client-config-loader_test.cpp).
- Response decompression and `Accept-Encoding`: [client-accept-encoding_test.cpp](../../aeronet/client/test/client-accept-encoding_test.cpp).
