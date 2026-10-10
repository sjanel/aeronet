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

An `HttpResponse` can also be constructed directly, without request: `HttpResponse(404)`, `HttpResponse("body")`, `HttpResponse(201, "body", "application/json")`. Setters return the response, so calls chain, also on temporaries:

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
| `bodyStatic(std::string_view, type)` | Data that outlives the response, such as a `static constexpr` buffer | Referenced, never copied. |
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
  return req.makeResponse().body(std::move(payload), "application/octet-stream");  // moved, not copied
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

An owned body smaller than `HttpServerConfig::minCapturedBodySize` (1 KiB by default) is copied next to the response head, which costs less than handling a separate buffer. Larger owned and static bodies are sent from their own buffer, without copy.

The response body is buffered: its size is known, and it is sent with `Content-Length`. For bodies too large to build in memory, use a [streaming handler](streaming-responses.md).

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
- End-to-end responses, keep-alive, and empty bodies: [tests/http-core_test.cpp](../../tests/http-core_test.cpp).
- Handlers that throw: [tests/http-additional_test.cpp](../../tests/http-additional_test.cpp), [tests/http-routing_test.cpp](../../tests/http-routing_test.cpp) (async handlers), and [http2-protocol-handler_test.cpp](../../aeronet/http2/test/http2-protocol-handler_test.cpp) (HTTP/2).
