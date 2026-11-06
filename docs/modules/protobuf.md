# Protocol Buffers

With the `AERONET_ENABLE_PROTOBUF` CMake option (`ON` by default when aeronet is the top-level project), the server and `HttpClient` send and receive [Protocol Buffers](https://protobuf.dev) messages as HTTP bodies:

- `bodyProto(msg)` serializes a message into the body of a response (server) or of a request (client), in the binary encoding,
- `bodyAsProto<T>()` parses the body of a request (server) or of a response (client), in the binary encoding or in ProtoJSON, according to its `Content-Type`,
- `makeProtoResponse(msg)` makes a response in the encoding that the `Accept` header of the request prefers, binary or ProtoJSON,
- the bodies are compressed and decompressed like any other body: a protobuf response is compressed when the client accepts a coding, a compressed request body is decoded before the handler parses it.

The support is header-only: the aeronet libraries do not use protobuf, so they do not depend on its version. The messages are serialized and parsed by the protobuf runtime of your program, the one your generated code is built for. protobuf is never downloaded: install it (`libprotobuf-dev protobuf-compiler` on Debian / Ubuntu), or turn the option `OFF`. Every version from 3.21 is supported.

## Build your messages

Generate the C++ classes of your `.proto` files with `protoc`, and link them with aeronet. With CMake, `protobuf_generate()` adds the generated sources to a target:

```cmake
find_package(aeronet CONFIG REQUIRED)  # finds protobuf too, when aeronet was built with AERONET_ENABLE_PROTOBUF

add_library(shop_messages STATIC)
protobuf_generate(TARGET shop_messages LANGUAGE cpp PROTOS proto/shop.proto IMPORT_DIRS proto)
target_include_directories(shop_messages PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")  # shop.pb.h
target_link_libraries(shop_messages PUBLIC protobuf::libprotobuf)

add_executable(shop main.cpp)
target_link_libraries(shop PRIVATE shop_messages aeronet::aeronet)
```

Then include `<aeronet/http-protobuf.hpp>` in the files that serialize or parse messages. protobuf is expensive to compile, so the core headers only declare the protobuf helpers, and the umbrella headers (`<aeronet/aeronet.hpp>`) do not include it. Without it, calling a helper fails at link time with an undefined reference to it.

A complete server is in [examples/protobuf.cpp](../../examples/protobuf.cpp), with its [protobuf-shop.proto](../../examples/protobuf-shop.proto). The snippets below use `google.protobuf.Struct`, a message shipped with protobuf, so that they compile as is: generated messages are used the same way.

### Lite runtime

Messages generated with `option optimize_for = LITE_RUNTIME;` are linked with `libprotobuf-lite`, smaller, without descriptors nor reflection. Build aeronet with `-D AERONET_PROTOBUF_RUNTIME=lite` for them: the aeronet targets then link the lite runtime, as a program must not link both. The binary encoding works the same, but ProtoJSON is not available: `bodyProtoJson()` is not declared, and a ProtoJSON body is answered `415`. OpenTelemetry links the full runtime, so it cannot be enabled with the lite one.

## Server

`HttpRequestView::bodyAsProto<T>()` parses the body of the request into a message of type `T`. It returns a `std::expected<T, HttpResponse>`, whose error is the response to send. `HttpResponse::bodyProto()` sets the body:

```cpp
#include <aeronet/http-protobuf.hpp>
#include <google/protobuf/struct.pb.h>

#include <utility>

Router router;
router.setPath(http::Method::POST, "/echo", [](const HttpRequestView& request) {
  auto fields = request.bodyAsProto<google::protobuf::Struct>();  // std::expected<Struct, HttpResponse>
  if (!fields) {
    return std::move(fields.error());  // 415, 413 or 400 describing the error
  }
  (*fields->mutable_fields())["status"].set_string_value("received");
  return request.makeResponse(http::StatusCodeCreated).bodyProto(*fields);  // Content-Type: application/protobuf
});
```

`bodyAsProto(msg)` parses into an existing message instead, which is replaced: a message allocated on a `google::protobuf::Arena`, or reused from one request to the next. It returns a `std::expected<void, HttpResponse>`.

The request body is parsed in place, from the buffer where it was received (or decompressed), without copy. It is the whole body: like `body()`, `bodyAsProto()` cannot be used after `readBody()`. In an async handler, `co_await request.bodyAwaitable()` first.

## Client

`HttpRequest::bodyProto()` sets the body of a request, and `HttpResponse::bodyAsProto<T>()` parses the body of the response. Its error is a `ProtoParseError`, with a `code` (`ProtoParseErrc`) and a `message`:

```cpp
#include <aeronet/http-protobuf.hpp>
#include <google/protobuf/struct.pb.h>

#include <string>
#include <utility>

google::protobuf::Struct order;
(*order.mutable_fields())["item"].set_string_value("book");

HttpClient client;
auto request = client.makeRequest(http::Method::POST, "http://localhost:8080/echo");
request.bodyProto(order);
const HttpClientResult result = client.request(std::move(request));
if (result && result->status() == http::StatusCodeCreated) {
  auto echoed = result->bodyAsProto<google::protobuf::Struct>();  // std::expected<Struct, ProtoParseError>
  const std::string status = echoed ? echoed->fields().at("status").string_value() : echoed.error().message;
}
```

The response is decoded by the client before it is parsed (see [Compression and caching](../reference/client-configuration.md#compression-and-caching)), and `HttpClientConfig::requestCompression` compresses the request body.

## Media types

`bodyProto()` sends `application/protobuf`, the media type of [RFC 9996](https://www.rfc-editor.org/rfc/rfc9996), and `bodyProtoJson()` sends `application/protobuf+json; charset=utf-8`, which RFC 9996 requires for ProtoJSON. Both take another `Content-Type` as second argument, for a peer expecting `application/x-protobuf` for instance.

The parsers deduce the encoding of the body from its `Content-Type` (`ProtoFormatOf()`), case-insensitively:

| Encoding | `Content-Type` |
| --- | --- |
| Binary | `application/protobuf`, its aliases `application/x-protobuf` and `application/x-protobuffer`, `application/vnd.google.protobuf`, `application/proto` (Connect), `application/octet-stream`, or no `Content-Type` |
| ProtoJSON | `application/protobuf+json`, its alias `application/x-protobuf+json`, `application/json` |

Any other media type is unsupported. So is a parameter that RFC 9996 requires to reject: an `encoding` that is not the one of the media type (`binary` or `json`), a `version` other than `1`, and for ProtoJSON a `charset` other than `utf-8`. The other parameters are ignored.

An empty body is a message with all its fields at their default value, the binary encoding of such a message being empty. For the same reason, `bodyProto()` of such a message sets an empty body, without `Content-Type`, which the parsers read back as the same message.

## Errors

| Error (`ProtoParseErrc`) | Cause | Response of `bodyAsProto()` (server) |
| --- | --- | --- |
| `UnsupportedMediaType` | A `Content-Type` of no supported encoding (see above), or a ProtoJSON body with the lite runtime | `415 Unsupported Media Type`, with an `Accept` header listing the supported media types (RFC 9110 §15.5.16) |
| `TooLarge` | A body larger than `kProtoMaxBodySize` (2 GiB - 1 byte), the limit of the protobuf runtime | `413 Payload Too Large` |
| `Malformed` | An invalid binary encoding, invalid ProtoJSON (or a field it does not know), or more than 100 nested messages | `400 Bad Request` |
| `MissingRequiredFields` | `required` fields (proto2) are absent | `400 Bad Request` |

The body of the error response is the `message` of the error, in `text/plain`: `Invalid protobuf binary encoding of shop.Order`, `Missing required fields of shop.Order: id`, the message of the ProtoJSON parser...

`bodyProto()` throws `std::length_error` if the encoding of the message exceeds `kProtoMaxBodySize`, and `bodyProtoJson()` throws `std::runtime_error` if the serialization fails, for an `Any` of an unknown type for instance. The body is then unchanged.

To parse a body received otherwise, `ParseProto(body, msg, contentType)` does the same as `bodyAsProto()`, and returns a `std::expected<void, ProtoParseError>`. Without `contentType`, the body is binary.

## ProtoJSON

With the full runtime, `bodyAsProto()` parses ProtoJSON bodies, and `bodyProtoJson()` serializes a message as ProtoJSON, for clients without protobuf (a browser, `curl`):

```cpp
#include <aeronet/http-protobuf.hpp>
#include <google/protobuf/struct.pb.h>

#include <utility>

Router router;
router.setPath(http::Method::POST, "/echo-json", [](const HttpRequestView& request) {
  auto fields = request.bodyAsProto<google::protobuf::Struct>();  // binary or ProtoJSON
  if (!fields) {
    return std::move(fields.error());
  }
  return request.makeResponse().bodyProtoJson(*fields);  // Content-Type: application/protobuf+json; charset=utf-8
});
```

It is the [ProtoJSON format](https://protobuf.dev/programming-guides/json/) of protobuf, with its default options: field names in lower camel case (the parser also accepts the names of the `.proto` file), 64-bit integers as strings, enumerations by name, and the fields at their default value omitted. A field the message does not have is an error, while the binary parser keeps unknown fields. The body is serialized into a `std::string`, captured without copy.

`bodyProto()` and `bodyProtoJson()` send the encoding chosen by the handler. To let the client choose, see [Content negotiation](#content-negotiation).

## Content negotiation

`HttpRequestView::makeProtoResponse(msg)` makes a response whose body is `msg`, in the encoding that the `Accept` header of the request prefers: binary (`application/protobuf`), or ProtoJSON (`application/protobuf+json; charset=utf-8`) for a message of the full runtime. One handler then serves protobuf clients and browsers:

```cpp
#include <aeronet/http-protobuf.hpp>
#include <google/protobuf/struct.pb.h>

#include <utility>

Router router;
router.setPath(http::Method::POST, "/orders", [](const HttpRequestView& request) {
  auto order = request.bodyAsProto<google::protobuf::Struct>();  // binary or ProtoJSON, per Content-Type
  if (!order) {
    return std::move(order.error());
  }
  (*order->mutable_fields())["status"].set_string_value("received");
  return request.makeProtoResponse(http::StatusCodeCreated, *order);  // binary or ProtoJSON, per Accept
});
```

The negotiation (`NegotiateProtoFormat()`) follows RFC 9110 §12.5.1:

| `Accept` | Response |
| --- | --- |
| absent, `*/*`, `application/*`, `application/protobuf` | binary |
| `application/json`, `application/protobuf+json` | ProtoJSON |
| `application/protobuf;q=0.5, application/json` | ProtoJSON: the highest quality wins |
| `application/json, application/protobuf` | binary: a tie goes to the binary encoding |
| `*/*, application/protobuf;q=0` | ProtoJSON: the most specific range decides |
| `text/html`, `application/protobuf;version=2` | none acceptable: binary, or `406` (see below) |

- The media types of an encoding are the ones that the parsers recognize ([Media types](#media-types)), aliases included: `Accept: application/x-protobuf` selects the binary encoding. The response always has the canonical media type of its encoding, which RFC 9996 requires for ProtoJSON.
- The quality of an encoding is the one of the most specific range that matches it: a media type with parameters, then without, then `application/*`, then `*/*`. A quality of `0` excludes it.
- A media type with a parameter that the parsers reject (`version=2`, `encoding=json` for the binary type, a `charset` other than `utf-8` for ProtoJSON) matches no encoding: the client asks for something the server cannot send.

The response has a `Vary: Accept` header, so that caches keep one response per encoding (`Vary: accept, accept-encoding` once compressed). A message of the lite runtime has no ProtoJSON: its encoding does not depend on `Accept`, and the response has no `Vary: Accept`.

When the `Accept` header lists no media type of an encoding that can be sent, the response is binary, as RFC 9110 allows a server to disregard the header. With `ProtoNotAcceptablePolicy::Reject` as last argument, the response is `406 Not Acceptable` instead, with a `text/plain` body listing the supported media types:

```cpp
#include <aeronet/http-protobuf.hpp>
#include <google/protobuf/struct.pb.h>

Router router;
router.setPath(http::Method::GET, "/status", [](const HttpRequestView& request) {
  google::protobuf::Struct status;
  (*status.mutable_fields())["healthy"].set_bool_value(true);
  return request.makeProtoResponse(status, ProtoNotAcceptablePolicy::Reject);  // 406 for Accept: text/html
});
```

The message is built before `makeProtoResponse()` decides. To answer `406` before building an expensive message, call `NegotiateProtoFormat(request.headerValueOrEmpty(http::Accept), true)` first (`false` for a message without ProtoJSON): it returns `ProtoFormat::Unsupported` when nothing is acceptable.

`makeProtoResponse()` reserves the response buffer once, for its headers and the binary encoding, then serializes the message into it, like `bodyProto()`. It throws like `bodyProto()` and `bodyProtoJson()`.

## Compression

A protobuf response is compressed when it is finalized, like any other inline body (see [Compression](../guides/compression.md)): with the coding negotiated from `Accept-Encoding`, above `CompressionConfig::minBytes`, and only if it shrinks enough (`maxCompressRatio`, `0.6` by default). The binary encoding is already compact, and compresses much less than JSON. Sizes compressed with the default levels of zstd (3) and gzip (6), for orders of the [test messages](../../aeronet/test_support/protobuf/aeronet-test.proto.in):

| Order | Binary | zstd | gzip | ProtoJSON | zstd | gzip |
| --- | --- | --- | --- | --- | --- | --- |
| 10 items | 251 B | 75% | 72% | 559 B | 43% | 41% |
| 100 items | 2.5 KB | 30% | 30% | 5.1 KB | 12% | 17% |
| 1000 items | 26 KB | 20% | 23% | 52 KB | 8% | 15% |
| 100 items, random SKUs, prices and quantities | 2.7 KB | 80% | 82% | | | |
| 1000 items, random SKUs, prices and quantities | 27 KB | 76% | 78% | | | |

Text and repeated values compress well, while identifiers, hashes and measurements, close to random, do not: such messages are compressed for nothing at each response, the result being discarded above `maxCompressRatio`. Recommended settings for a protobuf API:

- Keep `minBytes` at its default of 1 KiB or more: smaller messages gain a few dozen bytes at best.
- If its messages are mostly numbers, either give up compressing the binary encoding with `contentTypeAllowList` (listing `application/protobuf+json` and the text types, without `application/protobuf`), or accept smaller gains with a `maxCompressRatio` of `0.8` to `0.9`, so that the CPU spent compressing is not wasted.
- Measure your own messages: compress a few typical bodies with `zstd -3` and `gzip -6`.

`contentTypeAllowList` compares media types without their parameters: list `application/protobuf`, and `application/protobuf+json` for ProtoJSON, to compress protobuf bodies when it is set.

Request bodies are decompressed by the server before the handler runs (see [Request body decompression](../guides/compression.md#request-body-decompression)), and by the client for responses: `bodyAsProto()` always sees the decoded body.

## Performance

- `bodyProto()` computes the size of the message (`ByteSizeLong()`), reserves it once in the buffer of the response, and serializes the message directly there (`SerializeWithCachedSizesToArray()`): there is no intermediate buffer, and the body is not copied before it is sent (or compressed).
- `bodyAsProto()` parses the body from the receive buffer, without copy. Parsing into a message allocated on an `Arena` (`bodyAsProto(msg)`) saves the allocations of its sub-messages and strings.
- ProtoJSON is much slower to serialize and parse than the binary encoding, and larger: keep it for clients that cannot use the binary one.

## Limits and security

- The protobuf runtime limits an encoding to 2 GiB - 1 byte, and the nesting of messages to 100 levels.
- A small binary body can make the parser allocate much more memory than its size: each empty sub-message of a `repeated` field takes 2 bytes of encoding, but a whole object once parsed. Lower `HttpServerConfig::maxBodyBytes` (256 MiB by default), and `DecompressionConfig::maxDecompressedBytes` for compressed bodies, to what your messages need.
- The binary encoding of a message with `map` fields is not deterministic: the order of the entries depends on the hash map. Do not compare encodings to compare messages.

## Tests

- Media types, parsing, errors, serialization of responses: [http-protobuf_test.cpp](../../aeronet/http/test/http-protobuf_test.cpp).
- `Accept` negotiation (`NegotiateProtoFormat()`), and the single allocation of `makeProtoResponse()` (`MakeProtoResponse.SingleAllocation`): [http-protobuf_test.cpp](../../aeronet/http/test/http-protobuf_test.cpp).
- Server and `HttpClient` over HTTP/1.1 and HTTP/2, ProtoJSON, compressed requests and responses, async handlers, `HEAD`, `makeProtoResponse()` (encodings, `406`, `Vary`): [http-protobuf-integration_test.cpp](../../tests/http-protobuf-integration_test.cpp).
