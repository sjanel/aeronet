# Streaming responses

A streaming handler writes its response piece by piece through an `HttpResponseWriter`, instead of returning a complete `HttpResponse`. It suits bodies too large to build in memory, such as an export generated row by row, without knowing their size in advance.

```cpp
#include <string>

Router router;
router.setPath(http::Method::GET, "/export", [](const HttpRequestView&, HttpResponseWriter& writer) {
  writer.status(http::StatusCodeOK);
  writer.contentType("text/csv");
  for (int row = 0; row < 100000; ++row) {
    if (!writer.writeBody(std::to_string(row) + ",value\n")) {
      break;  // the connection is closing: stop producing
    }
  }
  writer.end();
});
```

## How it works

- Set the status, the reason, and the headers (`header()`, `headerAddLine()`, `contentType()`) before the first write: the headers are sent with the first body bytes.
- Without length, the body is sent with chunked transfer encoding over HTTP/1.1, and as DATA frames over HTTP/2. Call `contentLength(n)` before the first write when the exact size is known: the response is then sent with `Content-Length` and without chunk framing, and you must write exactly `n` bytes.
- An HTTP/1.0 client does not understand chunked encoding (RFC 9112 §6.1): without length, its response has neither `Content-Length` nor `Transfer-Encoding`, and its body ends with the connection (RFC 9112 §6.3), which is closed after it even if the client asked to keep it alive. Its trailers are dropped. With `contentLength(n)`, the connection persists as for buffered responses.
- `writeBody()` sends the data, or queues it when the socket cannot take it yet. It returns `false` when the connection is closing: an error occurred, or the queued data exceeded `HttpServerConfig::maxOutboundBufferBytes` (over HTTP/2, the stream exceeded `Http2Config::maxStreamPendingBytes` and was reset). Stop writing then.
- `end()` completes the response. The server calls it when the handler returns without calling it, and further calls are ignored.
- Over HTTP/1.1, the head carries `Connection: close` when the connection closes after the response, as for buffered responses: the client asked for it, keep-alive is disabled, the request is the last one allowed by `maxRequestsPerConnection`, or the server drains. A drain that begins once the head was sent closes the connection after the complete response, without announcing it.
- For a `HEAD` request, the headers are sent and the body is discarded, so the same handler serves both methods. Without `contentLength()`, the `HEAD` response has no `Content-Length`, as the length of the chunked `GET` response is not known (see [Compression](compression.md#head-requests)).
- For a `1xx`, `204` or `304` status, which carry no content, the headers are sent without `Content-Length` nor `Transfer-Encoding`, and what the handler writes is discarded (see [Statuses without content](responses.md#statuses-without-content)).
- The body is compressed automatically, like buffered responses: the server buffers the first `minBytes`, then compresses each piece written. See [Compression](compression.md#streaming-responses).

!!! warning
    A streaming handler runs to completion on the event-loop thread, and `writeBody()` never waits for the client to read: data the client does not read yet accumulates in the connection's outbound queue. The handler therefore cannot wait between two writes without blocking the event loop, and memory is bounded by `maxOutboundBufferBytes` per connection. Streams that must stay open while events happen (server-sent events, progress notifications) are not supported by streaming handlers yet; use [WebSocket](../protocols/websocket.md) for them.

## Files

`writer.file(File, contentType)` sends a file as the whole body, with `sendfile()` on plaintext connections, and sets `Content-Length`. Call it instead of writing the body; it returns `false` if body data was already written. See [Static files](static-files.md) for a complete file server.

## Trailers

`trailerAddLine(name, value)`, called before `end()`, adds a trailer sent after the body, for values computed while writing it:

```cpp
#include <string>

Router router;
router.setPath(http::Method::GET, "/stream", [](const HttpRequestView&, HttpResponseWriter& writer) {
  writer.status(http::StatusCodeOK);
  std::size_t total = 0;
  for (int part = 0; part < 3; ++part) {
    const std::string chunk = "part-" + std::to_string(part) + "\n";
    total += chunk.size();
    writer.writeBody(chunk);
  }
  writer.trailerAddLine("x-total-bytes", std::to_string(total));
  writer.end();
});
```

Trailers need chunked encoding: they are ignored, with a warning, when `contentLength()` was set, and dropped for an HTTP/1.0 client. Unlike buffered responses, a streaming response does not announce its trailer names in a `trailer` header, as the headers are sent before the trailers are known.

## Errors

An exception escaping a streaming handler before anything was sent is answered with `500 Internal Server Error`, without body, like for other handlers (see [Errors](responses.md#errors)). The head is sent with the first body bytes, or with the first ones past the compression threshold.

Once the head was sent, the status cannot change anymore: when the handler throws, or the compression of the body fails, the server tells the client that the response is incomplete instead. Over HTTP/1.1, it closes the connection without completing the body (no final chunk, or fewer bytes than the `Content-Length`); over HTTP/2, it resets the stream with `INTERNAL_ERROR`. An HTTP/1.0 client receiving a body delimited by the close of the connection cannot tell it apart from a complete one. To report a failure that the client can tell apart, write a trailer before calling `end()`.

## Choosing a handler kind

| Need | Handler |
| --- | --- |
| A response computed quickly | [Synchronous](responses.md), returning `HttpResponse` |
| A large response produced in one go | Streaming, as on this page |
| A file | [Static file handler](static-files.md), or `HttpResponse::file()` |
| Waiting for a database or another service | [Async](async-handlers.md) |
| Messages pushed to the client over time | [WebSocket](../protocols/websocket.md) |

A path can mix streaming and buffered handlers for different methods, see [Dispatch precedence](routing.md#dispatch-precedence).

## Tests

- Chunked and fixed-length streaming, `HEAD`, trailers, and backpressure with slow readers (`HttpBackpressure` tests): [tests/http-core_test.cpp](../../tests/http-core_test.cpp).
- `Connection: close` announced for keep-alive disabled, the request limit and the client's close (`StreamingKeepAlive` tests): [tests/http-core_test.cpp](../../tests/http-core_test.cpp); for a drain beginning before or after the head: [tests/http-server-lifecycle_test.cpp](../../tests/http-server-lifecycle_test.cpp).
- Streaming handlers answering `204` or `304` (`HttpBodylessStatus` tests): [tests/http-core_test.cpp](../../tests/http-core_test.cpp) and [tests/http-protocol-parity_test.cpp](../../tests/http-protocol-parity_test.cpp).
- Streaming to an HTTP/1.0 client, close-delimited or with a `Content-Length` (`Http10` tests): [tests/http-additional_test.cpp](../../tests/http-additional_test.cpp).
- Streaming over HTTP/2: [tests/http2-core_test.cpp](../../tests/http2-core_test.cpp).
- Handlers throwing before or after the head was sent, and compression failing after it: [tests/http-additional_test.cpp](../../tests/http-additional_test.cpp) and [http2-protocol-handler_test.cpp](../../aeronet/server/test/http2-protocol-handler_test.cpp).
- Streaming compression: [tests/http-compression_test.cpp](../../tests/http-compression_test.cpp).
