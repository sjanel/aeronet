# Responses

A synchronous or async handler returns an `HttpResponse`: status, headers, body, and optional trailers, stored in a single buffer laid out as they will be sent. The server adds the framing headers (`Content-Length` or chunked encoding, `Date`, `Connection`), compresses the body when appropriate, and writes the response. Streaming handlers use an `HttpResponseWriter` instead, see [Streaming responses](streaming-responses.md).

## Building a response

`HttpRequestView::makeResponse()` is the preferred way to create a response: it pre-applies the server's global headers and the negotiated content coding, which saves work when the response is finalized, and enables [direct compression](compression.md#direct-compression).

```cpp
Router router;
router.setPath(http::Method::GET, "/hello", [](const HttpRequestView& req) {
  return req.makeResponse("hello\n");  // 200, text/plain
});
router.setPath(http::Method::POST, "/items", [](const HttpRequestView& req) {
  return req.makeResponse(http::StatusCodeCreated, R"({"id":42})", "application/json");
});
```

An `HttpResponse` can also be constructed directly, without request: `HttpResponse(404)`, `HttpResponse("body")`, `HttpResponse(201, "body", "application/json")`. `makeResponse()` and the constructors take the same bodies as the [body setters](#bodies): a string view, a C string or a byte span is copied, while an owned body (`std::string&&`, `std::vector<char>&&`, `std::vector<std::byte>&&`, or a `std::unique_ptr` buffer with its size) is moved without copy, so `HttpResponse(std::format(...))` does not copy the formatted string. Setters return the response, so calls chain, also on temporaries:

```cpp
#include <string>

Router router;
router.setPath(http::Method::GET, "/old", [](const HttpRequestView&) {
  return HttpResponse(http::StatusCodeMovedPermanently).location("/new");
});
router.setPath(http::Method::GET, "/report", [](const HttpRequestView& req) {
  HttpResponse response = req.makeResponse();
  response.headerAddLine("cache-control", "max-age=60").headerAddLine("x-request-id", "4f1c2a");
  response.body(std::string(1000, 'x'), "text/plain");
  return response;
});
```

For the best performance, fill a response in order: status and reason, then headers, then body, then trailers. Each step appends at the end of the buffer; going back, such as adding a header after the body, moves the bytes that follow.

## Status and headers

`status(code)` sets the status, and `reason(text)` an optional reason phrase. Header names are lower case, which literals are checked for at compile time:

| Method | Cost | Use it to |
| --- | --- | --- |
| `headerAddLine(name, value)` | No scan | Add a header. Duplicates are allowed: the cheapest way to add headers you know are not present yet. |
| `header(name, value)` | Scans existing headers | Set a header, replacing an existing one. |
| `headerAppendValue(name, value, sep)` | Scans existing headers | Append to a list header (`vary`, `cache-control`). |
| `headerRemoveLine(name)`, `headerRemoveValue(name, value)` | Scans existing headers | Remove a header (its last line), or one value of a list header. |
| `headerRemoveAllLines(name)` | Scans existing headers once | Remove every line of a repeated header (`set-cookie`...). |
| `headerValueOrEmpty(name)`, `headers()` | Scans existing headers | Read headers back. |
| `location(url)` | Scans existing headers | Set `Location`, for redirects. |

Some headers belong to the server and must not be set by handlers: `content-type` (set by the body setters), `content-length` (computed), `transfer-encoding`, `connection`, `date`, `te`, `trailer`, and `upgrade`. Setting `content-type` or `content-length` with a header method throws `std::invalid_argument`; setting the others is undefined behavior. `content-encoding` cannot be changed once a body is set (`std::logic_error`): set it before the body, see [Send your own encoding](compression.md#send-your-own-encoding).

### Reserved headers

The library reserves a small set of response headers that handlers cannot set directly on an `HttpResponse` (fixed responses) or on an `HttpResponseWriter` (streaming), because aeronet manages them itself, or a user-supplied value would be ambiguous or contradict what is actually sent. Attempting to set one asserts in debug builds, and is ignored in release builds for streaming responses:

- `date` - generated once per second and injected automatically.
- `content-length` - computed from the body (fixed responses) or set through `contentLength()` (streaming). This prevents inconsistencies between the declared and the actual size.
- `connection` - determined by the keep-alive policy (HTTP version, server configuration, request count, errors). Conflicting values supplied by user code could desynchronize the connection reuse logic.
- `transfer-encoding` - controlled by the streaming writer (`chunked`), or omitted when `content-length` is known. Arbitrary values risk illegal `content-length` + `transfer-encoding` combinations or unsupported encodings.
- `trailer` - emitted by the server when a response has trailers, to announce their names (unless `HttpServerConfig::addTrailerHeader` is `false`), see [Trailers](#trailers). A user-supplied value could contradict the trailers actually sent.
- `upgrade` - emitted by the server when it accepts a protocol switch, such as the `101 Switching Protocols` response of a [WebSocket](../protocols/websocket.md) handshake. A handler cannot ask for a switch by setting it.
- `te` - a request header (hop-by-hop transfer coding negotiation), meaningless in a response.

Convenience helpers remain allowed: `contentType()` on the streaming writer, and `location()` for redirects.

All other headers (custom application, caching, CORS, ...) may be set freely. Values are forwarded unchanged, while field names are normalized to lower case. Lower-case field names are valid in HTTP/1.x and mandatory in HTTP/2; the API intentionally does not preserve cosmetic HTTP/1.x capitalization. This rule lives in a single helper, `http::IsReservedResponseHeader`.

### Global headers

`HttpServerConfig::globalHeaders` are added to every response of the server, including the error responses the server generates itself (`400`, `404`, `413`, ...). A global header never overrides a header of the same name set by the handler. It contains `server: aeronet` by default:

```cpp
HttpServerConfig config;
config.withGlobalHeaders({});  // remove the default server header
config.addGlobalHeader({"strict-transport-security", "max-age=31536000"});
config.addGlobalHeader({"x-content-type-options", "nosniff"});
```

Responses created with `makeResponse()` start with the global headers already in place, so they need not be inserted before the body when the response is finalized.

## Bodies

Choose the body setter by who owns the data:

| Setter | Data | Copy |
| --- | --- | --- |
| `body(std::string_view, type)` | Any bytes, including request data | Copied into the response buffer. |
| `body(std::string&&, type)`, `body(std::vector<char>&&, type)`, `body(std::unique_ptr<char[]>, size, type)` | A buffer the handler owns | Moved, never copied. Use it for large bodies. |
| `bodyStatic(std::string_view, type)` | Data that outlives the response, such as a `static constexpr` buffer | Referenced, never copied. With [`MSG_ZEROCOPY`](../operations/performance.md#msg_zerocopy), the kernel reads it until the client acknowledged it: it must stay unchanged. |
| `bodyAppend(data)` | Successive pieces | Appended, with exponential buffer growth. |
| `bodyInlineSet(maxLen, writer)`, `bodyInlineAppend(maxLen, writer)` | Data written by a callback into the response buffer | Written in place: the callback receives a pointer to at least `maxLen` bytes and returns the number written. |
| `bodyJson(obj)`, `bodyYaml(obj)` | A C++ object | Serialized in place, see [JSON and configuration files](../modules/json-and-config-files.md#json-and-yaml-bodies). |
| `file(File, type)`, `file(File, offset, length, type)` | An open file | Sent from the file, with `sendfile()` on plaintext connections. See [Static files](static-files.md). |

```cpp
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

Router router;
router.setPath(http::Method::GET, "/large", [](const HttpRequestView& req) {
  std::string payload(4U << 20U, 'a');  // built by the handler
  return req.makeResponse(std::move(payload), "application/octet-stream");  // moved, not copied
});
router.setPath(http::Method::GET, "/inline", [](const HttpRequestView& req) {
  HttpResponse response = req.makeResponse();
  response.bodyInlineSet(64, [](char* out) -> std::size_t {
    static constexpr std::string_view kText = "written in place\n";
    std::memcpy(out, kText.data(), kText.size());
    return kText.size();
  });
  return response;
});
```

An owned body smaller than `HttpServerConfig::minCapturedBodySize` (1 KiB by default) is copied next to the response head, which costs less than handling a separate buffer. Larger owned and static bodies are sent from their own buffer, without copy, over HTTP/1.1 and HTTP/2.

`bodyAppend()` and `bodyInlineAppend()` also extend a moved or static body, at the end of its own buffer. A moved `std::string` or vector keeps its buffer, so its data is not copied unless it reallocates (`bodyInlineAppend()` zero-fills the `maxLen` new bytes of a vector before calling the writer, as a vector cannot hold uninitialized bytes). A static body or a `std::unique_ptr` buffer cannot grow: it is copied once into a buffer of the response, which further appends extend. Appending to a `file()` body throws `std::logic_error`.

The response body is buffered: its size is known, and it is sent with `Content-Length`. For bodies too large to build in memory, use a [streaming handler](streaming-responses.md).

## Operation costs

The methods that build an `HttpResponse` edit a single contiguous buffer holding the status line, the headers, and the inline body, so their cost depends on how much has to move:

| Operation | Complexity | Notes |
| --- | --- | --- |
| `status()` | O(1) | Overwrites 3 digits. |
| `reason()` | O(trailing) | One tail `memmove` if the size changes. |
| `header()` | O(headers + bodyLen) | Linear scan, and maybe one shift. |
| `headerAddLine()` | O(bodyLen) | Shifts the tail once; no scan. |
| `headerRemoveLine()` | O(headers + bodyLen) | Linear scan (in reverse), and maybe one shift. |
| `headerRemoveValue()` | O(headers + bodyLen) | Linear scan (in reverse), and maybe one shift. |
| `headerRemoveAllLines()` | O(headers + bodyLen) | Single scan, one shift per removed line, plus the tail. |
| `body()` (inline) | O(delta) + realloc | Exponential growth strategy. |
| `body()` (capture) | O(1) | Zero-copy capture of the client buffer. |
| `bodyStatic()` (capture) | O(1) | Zero-copy capture of the client buffer. |
| `bodyAppend()` (inline) | O(delta) + realloc | Exponential growth strategy, zero-copy support. |
| `bodyInlineAppend()` | O(delta) + realloc | Exponential growth strategy. |
| `bodyInlineSet()` | O(1) + realloc | Exact growth strategy. |
| `file()` | O(1) | Zero-copy `sendfile` helper. |
| `trailerAddLine()` | O(1) | Append-only; no scan (only after the body). |

Usage guidelines:

- Use `headerAddLine()` when duplicates are acceptable, or impossible from the client code: it is the cheapest path.
- Use `header()` only when you must guarantee uniqueness. Its key, like all header lookup and removal keys, must be lower case; lower-case string literals are checked at compile time through `LowerAsciiKey`.
- `headerAddLine()` and `trailerAddLine()` also require lower-case names through `LowerAsciiKey`, so header and trailer iteration and flat views always expose lower-case names.
- Chain calls on temporaries for concise construction: the rvalue-qualified overloads keep the object movable.
- For maximum performance, fill the response in order, starting with status and reason, then headers, then body and trailers, to minimize memory shifts and reallocations.

## Trailers

Trailers are header fields sent after the body, typically checksums or signatures. Add them after the body:

```cpp
Router router;
router.setPath(http::Method::GET, "/data", [](const HttpRequestView& req) {
  HttpResponse response = req.makeResponse("payload");
  response.trailerAddLine("x-checksum", "abc123");
  return response;
});
```

Adding a trailer before the body throws `std::logic_error`. Over HTTP/1.1, trailers require chunked encoding: the server converts the response to `Transfer-Encoding: chunked` when it is finalized, and announces the trailer names in a `trailer` header, unless `HttpServerConfig::addTrailerHeader` is `false`. Over HTTP/2, trailers are sent as a final `HEADERS` frame. Trailer names are not validated: do not send fields that frame, route, or authenticate messages (`content-length`, `content-type`, `transfer-encoding`, `host`, `authorization`, `set-cookie`, ...), which clients and intermediaries reject or misinterpret.

## Errors

An exception escaping a handler is caught by the server, logged with its message (`what()`), and answered with `500 Internal Server Error`, without body: the message, which can reveal internal details such as file paths or connection strings, is never sent to the client. Return explicit error responses for expected failures instead, with a body that helps the client, and keep exceptions for bugs.

## Tests

- Response building, headers, bodies, and trailers: [http-response_test.cpp](../../aeronet/http/test/http-response_test.cpp).
- `makeResponse()`, including moved bodies and `HEAD` requests: [http-request-view_test.cpp](../../aeronet/http/test/http-request-view_test.cpp).
- End-to-end responses, keep-alive, and empty bodies: [tests/http-core_test.cpp](../../tests/http-core_test.cpp).
- Handlers that throw: [tests/http-additional_test.cpp](../../tests/http-additional_test.cpp), [tests/http-routing_test.cpp](../../tests/http-routing_test.cpp) (async handlers), and [http2-protocol-handler_test.cpp](../../aeronet/server/test/http2-protocol-handler_test.cpp) (HTTP/2).
