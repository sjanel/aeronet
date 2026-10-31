# Requests

A handler receives the request as an `HttpRequestView`: a set of non-owning views into the connection buffer, parsed once and never copied. This page describes what a request exposes, how long its data lives, and how bodies are received.

## What a request exposes

| Accessor | Content |
| --- | --- |
| `method()`, `version()` | Method and HTTP version. |
| `path()` | Path, percent-decoded. |
| `pathParamValue(name)`, `pathParamValueOrEmpty(name)`, `pathParams()` | Parameters captured by the route pattern, see [Routing](routing.md#reading-parameters). |
| `queryParamValue(key)`, `queryParamValueOrEmpty(key)`, `queryParamInt<T>(key)`, `queryParams()`, `queryParamsRange()` | Query parameters, see below. |
| `headerValue(name)`, `headerValueOrEmpty(name)`, `hasHeader(name)`, `headers()` | Header fields. |
| `body()`, `readBody()`, `hasMoreBody()` | Body, see [Bodies](#bodies). |
| `trailers()`, `trailerValueOrEmpty(name)` | Trailer fields of a chunked (HTTP/1.1) or HTTP/2 request. |
| `clientAddress()` | Peer address of the connection, as text. |
| `authority()` | Host and optional port the request is for, when its request target gives it: the `:authority` of an HTTP/2 request, or the authority of an absolute-form HTTP/1.1 target, see [Request target forms](#request-target-forms). Empty otherwise: use the `host` header field. |
| `isHttp2()`, `streamId()`, `scheme()` | HTTP/2 specifics, see [HTTP/2](../protocols/http2.md#handlers). |
| `alpnProtocol()`, `tlsVersion()`, `tlsCipher()` | TLS parameters of the connection, empty on plaintext connections. |
| `reqStart()` | Time the request head started arriving. |

`headerValue()` and `pathParamValue()` return an empty `std::optional` when the field is absent, while the `...OrEmpty()` variants return an empty view, which cannot be told apart from a present but empty field.

### Header names

Header names are normalized to lower case while parsing, and lookups take lower-case names: `req.headerValueOrEmpty("x-request-id")`. A literal with upper-case letters fails to compile; a name built at run time must be lower-cased first. `http::ContentType`, `http::Accept`, and the other constants of `<aeronet/http-constants.hpp>` are already lower case.

When a header appears several times, the server combines the occurrences while parsing, without allocation, according to the semantics of the field:

| Field | Duplicates are | Examples |
| --- | --- | --- |
| List fields | joined with `,` | `accept`, `accept-encoding`, `via`, `te` |
| `cookie` | joined with `;` | `cookie` |
| `user-agent` | joined with a space | `user-agent` |
| Singleton fields | rejected with `400 Bad Request` | `authorization`, `proxy-authorization`, `content-type`, `range`, `if-range`, `if-modified-since`, `if-unmodified-since`, `referer`, `from`, `max-forwards` |
| Framing and routing fields | rejected with `400 Bad Request` | `content-length`, `host`, `content-md5`, `content-transfer-encoding`, `http2-settings`, `upgrade-insecure-requests` |
| Unknown fields | joined with `,` | custom `x-...` headers |

Rejecting duplicate singleton, `content-length` and `host` fields closes classic request smuggling vectors, where two intermediaries pick different occurrences: keeping the last `authorization`, `content-type` or `range` could differ from a proxy in front keeping the first (RFC 9110 §5.3 lets a recipient reject them). Over HTTP/2, the stream is reset with `PROTOCOL_ERROR` (malformed request, RFC 9113 §8.1.1). For the other fields, an empty occurrence does not add a separator. To reject, rather than merge, duplicates of unknown fields that your application treats as singletons, set `HttpServerConfig::mergeUnknownRequestHeaders` to `false`.

Tests: `DuplicateSingletonHeadersProduce400` in [http-request-view_test.cpp](../../aeronet/http/test/http-request-view_test.cpp), `DuplicateSingletonTrailersRejected` in [http-core_test.cpp](../../tests/http-core_test.cpp), `DuplicateSingletonHeaderIsRejected` in [hpack_test.cpp](../../aeronet/http2/test/hpack_test.cpp), and the table in [mergeable-headers_test.cpp](../../aeronet/objects/test/mergeable-headers_test.cpp).

### Header values

Header values are trimmed of their leading and trailing spaces and tabs. Besides visible ASCII characters, spaces and tabs, they may contain the bytes `0x80` to `0xFF` (`obs-text`, RFC 9110 §5.5): UTF-8 or ISO-8859-1 text, such as a raw UTF-8 file name in `content-disposition`. The server does not decode them: `headerValue()` returns the bytes as they were received. Any other control character (CR, LF, NUL, DEL...) makes the request a `400 Bad Request`. The same rules apply to trailers and to chunk extensions. Over HTTP/2, only NUL, CR, LF and leading or trailing whitespace are forbidden (RFC 9113 §8.2.1).

The header values aeronet sends are stricter: visible ASCII characters, spaces and tabs only, as RFC 9110 recommends for new fields. Copying a received value with other bytes into a response header throws `std::invalid_argument`, answered `500` when the handler does not catch it. Encode such a value first, for instance with the notation of RFC 8187 for `content-disposition`: `attachment; filename*=UTF-8''r%C3%A9sum%C3%A9.pdf`.

### Line endings

The request line and the header lines end with CRLF. RFC 9112 §2.2 lets a server accept a bare LF as line terminator, which aeronet does not: a line left without CRLF that holds a bare LF can never be completed, and is answered `400 Bad Request` as soon as received, instead of waiting for a timeout (`headerReadTimeout`, then `keepAliveTimeout`). The same holds for the lines of a chunked body (see [Chunked bodies and trailers](#chunked-bodies-and-trailers)) and for `TRACE` requests. A CR whose LF is received in the next read is waited for. A request head received at once is not scanned any further for this: the check only runs when a line is incomplete.

Tests: `BareLf*` and `CrLfSplitBetweenReadsIsNotABareLf` in [http-parser_test.cpp](../../aeronet/server/test/http-parser_test.cpp), `BareLfIn*` in [http-request-view_test.cpp](../../aeronet/http/test/http-request-view_test.cpp), `HeadWithABareLfDoesNotWait` in [trace-echo_test.cpp](../../aeronet/server/test/trace-echo_test.cpp), `TraceWithBareLfLineEndingsIsRejectedWithoutWaiting` in [http-core_test.cpp](../../tests/http-core_test.cpp).

### Query parameters

The query string is decoded once: `%XX` escapes are decoded and `+` becomes a space, in keys and values. A parameter without `=` has an empty value. A malformed escape is kept literally rather than rejected.

- `queryParams()` and `queryParamValue(key)` look parameters up by key. When a key appears several times, the last occurrence wins.
- `queryParamsRange()` iterates over all parameters in their order, duplicates included, for parameters that are lists (`?tag=a&tag=b`).
- `queryParamInt<T>(key)` parses an integer value, and returns an empty `std::optional` when the parameter is absent or not a valid integer.

```cpp
#include <string_view>

Router router;
router.setPath(http::Method::GET, "/search", [](const HttpRequestView& req) {
  const std::string_view text = req.queryParamValueOrEmpty("q");
  const int limit = req.queryParamInt<int>("limit").value_or(20);
  for (const auto& [key, value] : req.queryParamsRange()) {
    // every parameter, in order, duplicates included
  }
  return HttpResponse(200);
});
```

The path is decoded once too. A request target with an invalid escape, a decoded NUL byte (`%00`), or control characters is rejected with `400 Bad Request`.

### Request target forms

An HTTP/1.1 request target is accepted in the four forms of RFC 9112 §3.2: origin-form (`/path?query`, the usual one), absolute-form (`GET http://example.com/path?query HTTP/1.1`), authority-form (`CONNECT` only, see [CONNECT](../protocols/connect.md)) and asterisk-form (`OPTIONS *` only). Any other target is rejected with `400 Bad Request`.

With an absolute-form target, `path()` and the query parameters are the ones of its path and query: the request is routed as if it had been sent in origin-form, an empty path being `/`. The host is taken from the target, not from the `Host` header field (RFC 9112 §3.3): `authority()` and `headerValueOrEmpty("host")` both return its host and optional port, without userinfo, whatever the value of `Host`. This holds for the [HTTP to HTTPS redirect](../protocols/tls.md#http-to-https-redirect) and the WebSocket origin check too. The scheme of the target is not used, and an HTTP/1.1 request still needs exactly one valid `Host` header field. A target without authority (`mailto:a@b`, `http:/path`), with a fragment or control characters, or whose host and port, after the userinfo, are not a valid `Host` value (see below, the host cannot be empty here) is rejected with `400 Bad Request`.

Tests: `AbsoluteForm*` in [http-request-view_test.cpp](../../aeronet/http/test/http-request-view_test.cpp), and `AbsoluteFormTargetIsRoutedByItsPathAndHost` in [http-errors_test.cpp](../../tests/http-errors_test.cpp).

### Host header field

The `Host` header field must be `uri-host [ ":" port ]` (RFC 9110 §7.2), as must the `:authority` and `host` fields of an HTTP/2 request:

- the host is a name or an IPv4 address made of letters, digits, `-._~!$&'()*+,;=` and `%` escapes (an RFC 3986 reg-name), or an IPv6 address in brackets (`[::1]`, `[::ffff:192.0.2.1]`, or an `IPvFuture` such as `[v1.x]`);
- the optional port, after a `:`, is made of digits only.

Any other value, like `a b/c@d`, `evil.example/x` or `user@host`, is rejected with `400 Bad Request` before routing (RFC 9112 §3.2), so that handlers, the [HTTP to HTTPS redirect](../protocols/tls.md#http-to-https-redirect) and the WebSocket origin check can build URLs from it as is. The value is kept unchanged: its case is preserved and its `%` escapes are not decoded. An empty `Host`, which a client sends for a target URI without authority, is accepted. A host name with non-ASCII characters must be sent in its ASCII form (punycode).

Tests: `InvalidHostHeaderFieldIsRejected` and `ValidHostHeaderFieldIsKeptAsIs` in [http-request-view_test.cpp](../../aeronet/http/test/http-request-view_test.cpp), `InvalidAuthorityOrHostReturns400` in [http2-protocol-handler_test.cpp](../../aeronet/server/test/http2-protocol-handler_test.cpp), the syntax itself in [uri-host_test.cpp](../../aeronet/objects/test/uri-host_test.cpp).

## Lifetime of request data

Every view of the request (path, parameters, headers, body, trailers) points into the connection's buffer. The server guarantees that this buffer stays valid and unchanged until the handler returns, and only reuses it for the next request of the connection afterwards. Views can therefore be used freely during the handler, without copies:

```cpp
#include <string>
#include <string_view>

Router router;
router.setPath(http::Method::GET, "/users/{id}", [](const HttpRequestView& req) {
  const std::string_view userId = req.pathParamValueOrEmpty("id");  // valid until the handler returns
  return HttpResponse(200).body(std::string("user ") + std::string(userId));
});
```

Data that must outlive the handler, stored in a cache, captured by a callback, or sent to another thread, must be copied first: `std::string(view)`.

- **Async handlers** keep their request valid across suspensions: when a handler suspends, the server moves the request head into a buffer pinned for that request, and does not read the next request of the connection until the handler completes. See [Async handlers](async-handlers.md#lifetime-and-thread-safety-rules).
- **Responses** may be built from request views with `body(std::string_view)`, which copies its argument. `bodyStatic()` references its argument without copying: never pass it request data.

## Bodies

`body()` returns the complete body. Before calling a synchronous or streaming handler, the server receives the whole body, removes chunked framing, and decodes it when it is compressed (see [Request body decompression](compression.md#request-body-decompression)). Async handlers can start before the body has arrived and wait for it with `co_await req.bodyAwaitable()`.

`readBody(maxBytes)` and `hasMoreBody()` read the same body in successive slices, for code written for incremental consumption. `body()` and `readBody()` cannot be mixed on the same request.

### Limits and timeouts

| Setting | Default | When exceeded |
| --- | --- | --- |
| `maxHeaderBytes` | 8 KiB | `431 Request Header Fields Too Large`, includes request trailers. A request line too long for it, whose request target is too long, is answered `414 URI Too Long` (RFC 9112 §3) instead, whether or not its end was received. |
| `maxBodyBytes` | 256 MiB | `413 Payload Too Large`, before the body is read when a `Content-Length` announces it (the connection then closes after a [lingering close](server-lifecycle.md#lingering-close)). |
| `headerReadTimeout` | disabled | `408 Request Timeout` and connection closed: protects against clients sending their head byte by byte (Slowloris). Counted from the accept for the first request of a connection, from its first byte for the next ones. |
| `bodyReadTimeout` | disabled | `408 Request Timeout` and connection closed when the body stops arriving. |

Routes can lower the size limits, and set a deadline for the whole request: see [Per-route options](routing.md#per-route-options). Request bodies are buffered entirely before the handler reads them: there is no streaming of request bodies to handlers yet, so `maxBodyBytes` also bounds the memory a request can use. Set it, and the timeouts, according to what your endpoints need.

### Chunked bodies and trailers

HTTP/1.1 clients may send a body without knowing its size, with `Transfer-Encoding: chunked`. The server decodes it (RFC 9112 §7.1) before the handler sees it:

- Chunk sizes are strict hexadecimal numbers. Empty sizes, prefixes such as `0x`, `+`, or `-`, whitespace (except before the `;` of a chunk extension), sizes overflowing 64 bits, and bare CR or LF characters are rejected with `400 Bad Request` (a chunk-size line, the line ending the data of a chunk or the body, or a trailer line ending with a bare LF is answered as soon as received, not once a timeout expires), and every framing error closes the connection, so that the following bytes are never parsed as another request.
- Chunk extensions (`4;name=value`, or `4 ; name=value`: spaces and tabs are allowed around the semicolon) are validated and ignored. A chunk-size line (extensions and CRLF included) longer than `maxHeaderBytes` is rejected with `400`, whether or not its end was received: a line without end is not buffered further.
- The decoded body counts toward `maxBodyBytes` (`413` beyond it, as soon as a chunk announces the excess, without waiting for its data), and the trailer section toward `maxHeaderBytes` (`431`, whether or not the end of its last line was received).
- A body received in several reads is decoded once: each read decodes the chunks received since the previous one, so the time spent is linear in the size of the body.
- `Transfer-Encoding` on an HTTP/1.0 request, together with `Content-Length`, or whose final coding is not `chunked` (RFC 9112 §6.3), is rejected with `400`. Other transfer codings before `chunked` (`gzip, chunked`) are not supported: `501 Not Implemented`.

Trailers are header fields sent after the last chunk, typically a checksum or a signature computed while sending the body. They are exposed by `trailers()`, with lower-case names and trimmed values, identically for HTTP/1.1 chunked requests and HTTP/2 requests:

```cpp
#include <string_view>

Router router;
router.setPath(http::Method::PUT, "/objects/{key}", [](const HttpRequestView& req) {
  const std::string_view body = req.body();
  const std::string_view checksum = req.trailerValueOrEmpty("x-checksum");
  // verify the checksum of body...
  return HttpResponse(204);
});
```

Fields that would change how the message is framed, routed, or authenticated are forbidden in trailers, and a request carrying one is rejected with `400`: `authorization`, `proxy-authorization`, `proxy-authenticate`, `www-authenticate`, `transfer-encoding`, `content-length`, `content-range`, `content-encoding`, `content-type`, `host`, `cache-control`, `expect`, `max-forwards`, `pragma`, `range`, `te`, `trailer`, `set-cookie`, and `cookie`.

### Forms and file uploads

`MultipartFormData` parses a `multipart/form-data` body (RFC 7578), as sent by HTML forms with file inputs, into parts that are views of the request body:

```cpp
#include <aeronet/multipart-form-data.hpp>

#include <string>
#include <utility>

Router router;
router.setPath(http::Method::POST, "/upload", [](const HttpRequestView& req) {
  const MultipartFormData form(req.headerValueOrEmpty(http::ContentType), req.body());
  if (!form.valid()) {
    return HttpResponse(400, std::string("invalid form: ") + std::string(form.invalidReason()));
  }
  if (const MultipartFormData::Part* file = form.part("file"); file != nullptr && file->hasFilename()) {
    // *file->filename is the client-side name, file->value the content, file->contentType its type if sent
  }
  return HttpResponse(204);
});
```

Each part exposes its `name`, an optional `filename` and `contentType`, its `value`, and its own headers (`headers()`, `headerValueOrEmpty()`). `part(name)` returns the first part with that name, `parts(name)` all of them, and `parts()` every part. Never use `filename` as a path on disk without sanitizing it: it is chosen by the client.

`MultipartFormDataOptions`, passed as a third argument, bounds the parsing:

| Option | Default | Effect |
| --- | --- | --- |
| `maxParts` | 128 | Maximum number of parts; 0 disables the check. |
| `maxHeadersPerPart` | 32 | Maximum number of header lines in a part. |
| `maxPartSizeBytes` | 32 MiB | Maximum size of a part's content; 0 disables the check. |

A malformed body (missing boundary, part without `Content-Disposition`, exceeded limit) leaves the form invalid, with a reason and no parts: the parser never throws. Quoted boundaries and the simple RFC 5987 form of `filename*=` are supported. The whole body is buffered first, so `maxBodyBytes` bounds the size of an upload.

## Expectations

A client that sends `Expect: 100-continue` waits for the server's `100 Continue` before sending a large body. aeronet sends it when it starts reading the body, so that a request rejected on its head, for instance because its `Content-Length` exceeds `maxBodyBytes`, is answered before the client sends its body. An HTTP/1.0 client cannot receive a `1xx` response: its `100-continue` expectation is ignored (RFC 9110 §10.1.1). Other expectations can be handled by an expectation handler, see [HTTP/1.1 conformance](../reference/http-conformance.md#expect).

## Tests

- Request parsing, header merging, and query decoding: [http-request-view_test.cpp](../../aeronet/http/test/http-request-view_test.cpp) and [http-parser_test.cpp](../../aeronet/server/test/http-parser_test.cpp).
- Header value validation, received (`obs-text` allowed) and sent: [http-header-is-valid_test.cpp](../../aeronet/objects/test/http-header-is-valid_test.cpp), and end to end, with the HTTP Garden cases of bytes above `0x7F` kept as is: [tests/http-errors_test.cpp](../../tests/http-errors_test.cpp).
- Chunked bodies and trailers end to end: [tests/http-core_test.cpp](../../tests/http-core_test.cpp).
- Chunked bodies received in several reads (byte by byte, large, compressed, async handler), and the limits of their lines: [http-parser_test.cpp](../../aeronet/server/test/http-parser_test.cpp), [http-request-decompression_test.cpp](../../tests/http-request-decompression_test.cpp) and [http-async_test.cpp](../../tests/http-async_test.cpp). The upload throughput is measured by `ChunkedUpload` in [http1-pipeline_bench.cpp](../../benchmarks/internal/http1-pipeline_bench.cpp).
- Multipart parsing: [multipart-form-data_test.cpp](../../aeronet/server/test/multipart-form-data_test.cpp).
