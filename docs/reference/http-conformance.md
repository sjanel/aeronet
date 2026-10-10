# HTTP/1.1 conformance

This page lists the HTTP/1.1 behaviors aeronet implements ([RFC 9110](https://www.rfc-editor.org/rfc/rfc9110) semantics, [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112) message syntax), the status codes the server produces by itself, and the protocol behaviors an application can configure. HTTP/2 is covered in [HTTP/2](../protocols/http2.md).

## Message parsing

| Behavior | Status |
| --- | --- |
| Request line parsing (method, target, version) | Supported |
| Header fields, looked up case-insensitively. Obsolete line folding is not accepted. | Supported |
| Method tokens are case-sensitive (RFC 9110 §9.1): `GET` is a method, `get` is an unknown method answered `501` | Supported |
| A bare CR in a request head is rejected (`400`) as soon as the byte following it is received | Supported |
| `Content-Length` and `Transfer-Encoding` together are rejected (`400`), as is `Transfer-Encoding` on an HTTP/1.0 request | Supported |
| Pipelined requests, processed one after the other | Supported |
| Duplicate request headers merged, overridden, or rejected depending on the header ([details](../guides/requests.md#header-names)) | Supported |
| Optional stricter policy failing on unknown duplicate headers | Not implemented |

## Connections

| Behavior | Status |
| --- | --- |
| Persistent connections: default for HTTP/1.1, opt-in with `Connection: keep-alive` for HTTP/1.0 | Supported |
| HTTP/1.0 requests answered with HTTP/1.0 responses (no silent upgrade) | Supported |
| `Connection: close` from either side | Supported |
| Requests per connection limit (`maxRequestsPerConnection`) and idle timeout (`keepAliveTimeout`) | Supported |
| Header read timeout against slow clients (Slowloris), disabled by default | Supported |
| Partial writes and backpressure, bounded by `maxOutboundBufferBytes` | Supported |
| `CONNECT` tunneling | Supported, see [CONNECT tunneling](../protocols/connect.md) |
| Upgrade to WebSocket | Supported, see [WebSocket](../protocols/websocket.md) |
| Upgrade to cleartext HTTP/2 (`Upgrade: h2c`) | Ignored, as RFC 9113 deprecated it |

## Request bodies

| Behavior | Status |
| --- | --- |
| `Content-Length` bodies, bounded by `maxBodyBytes` | Supported |
| Chunked transfer coding, with chunk extensions and trailers ([details](../guides/requests.md#chunked-bodies-and-trailers)) | Supported |
| Request trailers carrying forbidden fields rejected | Supported |
| Trailers exposed by `HttpRequestView::trailers()`, identically for HTTP/1.1 and HTTP/2 | Supported |
| Request body decompression (`Content-Encoding`: gzip, deflate, br, zstd, stacked codings), with size and ratio limits ([details](../guides/compression.md#request-body-decompression)) | Supported, opt-in |
| `multipart/form-data` parsing helpers ([details](../guides/requests.md#forms-and-file-uploads)) | Supported |
| `Expect: 100-continue`, and custom expectations through a handler (see below) | Supported |

## Responses

| Behavior | Status |
| --- | --- |
| `Date`, `Content-Length`, `Connection`, `Transfer-Encoding`, `Trailer`, `TE`, and `Upgrade` response headers managed by the server ([details](../guides/responses.md#status-and-headers)) | Supported |
| `Content-Length: 0` on empty responses, except for `1xx`, `204`, `304`, `HEAD`, file, streaming, and direct-compression responses | Supported |
| `HEAD` answered by the `GET` handler, without body and with the `GET` length | Supported |
| Chunked streaming responses, with trailers | Supported |
| Buffered responses with trailers, sent chunked | Supported |
| Response compression (gzip, deflate, br, zstd) negotiated from `Accept-Encoding` with q-values ([details](../guides/compression.md)) | Supported |
| `406 Not Acceptable` when the client refuses `identity` and no offered coding is available | Supported |
| Range and conditional requests for files ([details](../guides/static-files.md)) | Supported |

## Status codes produced by the server

The server answers these errors itself, before or instead of calling a handler:

| Status | Cause |
| --- | --- |
| `400 Bad Request` | Malformed request, conflicting framing headers, `Transfer-Encoding` on HTTP/1.0, invalid `CONNECT` target, failed WebSocket handshake. |
| `403 Forbidden` | `CONNECT` target not in the allowlist, request denied by a CORS policy. |
| `404 Not Found` | No route matches the path. |
| `405 Method Not Allowed` | The path exists but not for this method, with an `Allow` header. Also `TRACE` when disabled. |
| `406 Not Acceptable` | No acceptable content coding. |
| `413 Payload Too Large` | Body larger than `maxBodyBytes`. |
| `415 Unsupported Media Type` | Unsupported `Content-Encoding` on a request body, when decompression is enabled. |
| `417 Expectation Failed` | `Expect` token other than `100-continue` without expectation handler. |
| `431 Request Header Fields Too Large` | Request head larger than `maxHeaderBytes`. |
| `500 Internal Server Error` | Handler exception, or an invalid interim status returned by an expectation handler. |
| `501 Not Implemented` | Unknown method, or unsupported `Transfer-Encoding`. |
| `502 Bad Gateway` | `CONNECT` target unreachable. |
| `505 HTTP Version Not Supported` | Major version other than 1. A higher minor version, such as `HTTP/1.3`, is processed and answered as HTTP/1.1 (RFC 9110 §6.2). |

aeronet does not validate the `Content-Type` of request bodies: a `415` based on the media type is the application's decision, for instance in a request middleware.

## Configurable behaviors

### OPTIONS

`OPTIONS *` is answered `200` with an `Allow` header listing the methods the router supports. `OPTIONS` on a path is a CORS preflight when the route has a CORS policy, and is routed like any other method otherwise.

### TRACE

`TRACE` echoes the received request back to the client, with `Content-Type: message/http` (RFC 9110 §9.3.8). Because the echo includes every header, cookies and credentials included, it is disabled by default:

| `HttpServerConfig::TraceMethodPolicy` | Effect |
| --- | --- |
| `Disabled` (default) | `TRACE` is answered `405`. |
| `EnabledPlainAndTLS` | `TRACE` is echoed on every connection. |
| `EnabledPlainOnly` | `TRACE` is echoed on plaintext connections and answered `405` on TLS connections. |

```cpp
HttpServerConfig config;
config.withTracePolicy(HttpServerConfig::TraceMethodPolicy::EnabledPlainOnly);
```

### Expect

For `Expect: 100-continue`, the server sends `100 Continue` when it starts reading the body, so a client waiting for it does not send a body that would be refused. The token is recognized in a comma-separated list, with surrounding whitespace.

Other expectation tokens go to the expectation handler, when one is installed. Without a handler, they are answered `417 Expectation Failed`. The handler receives the request and the token, and returns an `ExpectationResult`:

| `ExpectationResultKind` | Effect |
| --- | --- |
| `Continue` | Process the request normally. |
| `Interim` | Send the informational response `interimStatus` (a `1xx` status, such as `102`), then continue. |
| `FinalResponse` | Send `finalResponse` and skip the body and the handler. |
| `Reject` | Answer `417 Expectation Failed`. |

```cpp
#include <string_view>

SingleHttpServer server(HttpServerConfig{}.withPort(8080));
server.setExpectationHandler([](const HttpRequestView& /*request*/, std::string_view token) {
  SingleHttpServer::ExpectationResult result;
  if (token == "x-quota-check") {
    result.kind = SingleHttpServer::ExpectationResultKind::Interim;
    result.interimStatus = 102;
  } else {
    result.kind = SingleHttpServer::ExpectationResultKind::Reject;
  }
  return result;
});
```

The handler runs on the event-loop thread, so it must be fast. An `Interim` result with a status outside `1xx` is a programming error, answered `500`. `MultiHttpServer::setExpectationHandler()` installs a handler on every worker.

## Tests

- Error statuses and version handling: [tests/http-errors_test.cpp](../../tests/http-errors_test.cpp).
- Keep-alive, pipelining, empty bodies: [tests/http-core_test.cpp](../../tests/http-core_test.cpp).
- `Expect`, `OPTIONS`, `TRACE`, and other protocol details: [tests/http-additional_test.cpp](../../tests/http-additional_test.cpp).
