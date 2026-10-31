# Compression

aeronet compresses responses and decompresses request bodies with gzip, deflate, zstd, and brotli. Each codec is a build option, and a compiled codec is used in both directions: compressing responses for clients that accept it, and decoding request bodies that use it. The [HTTP client](../modules/http-client.md#compression) uses the same codecs.

| Content coding | CMake option | Library |
| --- | --- | --- |
| `gzip`, `deflate` | `AERONET_ENABLE_ZLIB` | zlib-ng by default, classic zlib with `AERONET_ENABLE_ZLIBNG=OFF` |
| `zstd` | `AERONET_ENABLE_ZSTD` | Zstandard |
| `br` | `AERONET_ENABLE_BROTLI` | Brotli |

The three options default to `ON` when aeronet is the top-level project, and must be enabled explicitly when it is consumed as a dependency. Without any codec, responses are never compressed and request bodies are passed through as received.

## Response compression

Response compression is active as soon as a codec is compiled: no configuration is needed. For each response, the server:

1. negotiates a coding from the request's `Accept-Encoding` header;
2. compresses the body if it is large enough and of an allowed content type;
3. keeps the compressed body only if it is meaningfully smaller;
4. sets `Content-Encoding`, updates `Content-Length`, and adds `Vary: Accept-Encoding`, so that caches keep one version per coding.

`CompressionConfig`, passed to `HttpServerConfig::withCompression()`, tunes these steps:

```cpp
CompressionConfig compression;
compression.minBytes = 512;                // do not compress smaller bodies
compression.maxCompressRatio = 0.8F;       // keep the result if it saves at least 20%
compression.preferredFormats.push_back(Encoding::zstd);
compression.preferredFormats.push_back(Encoding::gzip);
compression.contentTypeAllowList.append("application/json");
compression.contentTypeAllowList.append("text/html");

HttpServerConfig config;
config.withCompression(std::move(compression));
```

| Setting | Default | Effect |
| --- | --- | --- |
| `minBytes` | 1024 | Bodies smaller than this are sent uncompressed. `std::numeric_limits<std::size_t>::max()` disables compression. |
| `maxCompressRatio` | 0.6 | The compressed body is kept only if its size is at most this fraction of the original (at least 40% smaller by default). |
| `contentTypeAllowList` | empty: every type | Content types eligible for compression, compared case-insensitively. |
| `preferredFormats` | empty | Codings preferred by the server, used to break ties (see below). |
| `addVaryAcceptEncodingHeader` | `true` | Add `Vary: Accept-Encoding` to compressed responses. |
| `defaultDirectCompressionMode` | `Auto` | See [Direct compression](#direct-compression). |
| `initialCompressionBufferLimit` | 32 KiB | Output buffer reserved upfront; larger bodies grow it while compressing. |
| `useLeadingZeroesInContentLength` | `true` | A body compressed when the response is finalized gets a `Content-Length` as wide as the largest accepted compressed size (`maxCompressRatio` of the body size), padded with leading zeroes (`content-length: 0333`), which avoids moving the body. [Direct compression](#direct-compression) writes the exact length, and streamed responses have none. Leading zeroes are valid HTTP (`1*DIGIT`), but a parser reading `010` as octal 8 disagrees with aeronet on where the body ends, which a proxy chain can turn into request smuggling: set `false` when such a parser may sit between the server and its clients. Also applies to the request bodies compressed by `HttpClient` (`requestCompression.codec`). |
| `zlib.level`, `zstd.compressionLevel`, `zstd.windowLog`, `brotli.quality`, `brotli.window` | library defaults | Codec tuning: higher levels compress better and cost more CPU. |

Already compressed content, such as images, video, or archives, gains nothing: restrict `contentTypeAllowList` to text types (HTML, JSON, JavaScript, CSS, SVG) when a server mixes both, to save the CPU of a compression attempt that would be discarded.

### Negotiation

The `Accept-Encoding` header lists the codings a client accepts, with optional quality values (`q`, from 0 to 1, default 1). The server picks the compiled coding with the highest quality; `gzip;q=0.5, br;q=0.9` selects `br`. A quality of 0 excludes a coding, and `*` stands for every coding not listed.

When several codings share the best quality, the server preference decides: the codings of `preferredFormats` in their order, then the other compiled codings in the default order, `zstd`, `br`, `gzip`, `deflate`, and finally the uncompressed form (`identity`). With `preferredFormats` set to `gzip` only, `Accept-Encoding: br, gzip` selects `gzip`, while `Accept-Encoding: br` still selects `br`.

Without `Accept-Encoding`, or when no compiled coding is acceptable, the response is sent uncompressed. If the client also excludes the uncompressed form (`identity;q=0`, or `*;q=0` without `identity`), the server answers `406 Not Acceptable`.

Each event loop remembers the result for the last `Accept-Encoding` value it negotiated, if it is not longer than that of the current browsers (`gzip, deflate, br, zstd`, 23 bytes): clients mostly send the value of their browser or HTTP library, so that the negotiation of most requests is a comparison with the previous one.

### What is not compressed

- Bodies smaller than `minBytes`, of a content type outside `contentTypeAllowList`, or that do not shrink enough.
- Responses that already have a `Content-Encoding` header (see [Send your own encoding](#send-your-own-encoding)).
- File bodies (`HttpResponse::file()`, the static file handler), which are sent with `sendfile()` or read from disk as they are. Store pre-compressed variants of large static assets if they need compression.

### HEAD requests

A `HEAD` response carries the headers of the `GET` response (RFC 9110 §9.3.2), and its `Content-Length` must be the length of the `GET` content (§8.6). When a coding is negotiated, the server therefore compresses the body of a `HEAD` response like the one of a `GET`, then drops it: the response gets the same `Content-Encoding`, `Vary` and `Content-Length`, including when the body does not shrink enough and is sent uncompressed. Without a negotiated coding, nothing is compressed, and a response made with `makeResponse()` only keeps the size of the body set by the handler.

A streaming handler answering a `HEAD` request takes the same decision as for a `GET` (`Content-Encoding` once `minBytes` are written), but the body is neither compressed nor sent. As the length of a chunked `GET` response is not known when its head is sent, the `HEAD` response has no `Content-Length`, unless the handler declared one with `contentLength()`.

### Streaming responses

For a response written with `HttpResponseWriter`, the server holds the headers and buffers the body until it reaches `minBytes`. It then decides once: either it starts a streaming encoder, adds `Content-Encoding`, and compresses each chunk as it is written, reusing one output buffer for the whole response, or the response ends before the threshold and is sent uncompressed with accurate headers.

### Direct compression

A response created with `HttpRequestView::makeResponse()` knows the negotiated coding, so it can compress its body while the handler writes it, in `body()` and `bodyAppend()`, instead of compressing a complete body when the response is finalized. This saves a pass over the body and a temporary buffer.

```cpp
Router router;
router.setPath(http::Method::GET, "/report", [](const HttpRequestView& req) {
  HttpResponse response = req.makeResponse();  // carries the negotiated coding
  response.body("a large text body, compressed as it is written...", "text/plain");
  return response;
});
```

| `DirectCompressionMode` | Behavior |
| --- | --- |
| `Auto` (default) | Compress inline when the request accepts a compiled coding, the first body chunk reaches `minBytes`, and its content type is allowed. |
| `On` | Same, without the size and content type conditions. |
| `Off` | Never compress inline. The response may still be compressed when finalized. |

Set the mode for all responses with `CompressionConfig::defaultDirectCompressionMode`, or for one response with `HttpResponse::directCompressionMode()`. Direct compression only applies to inline bodies: an owned body passed by value (`body(std::string)`, `body(std::vector<char>)`) is captured without copy and compressed when the response is finalized, and a file body is never compressed. When direct compression is active, the server manages `Content-Encoding` and `Vary` as the body changes, and the finalization step does not compress again.

### Send your own encoding

Setting `Content-Encoding` yourself disables automatic compression for that response, whatever its value. Use it to send content that is already compressed:

```cpp
#include <string>

std::string gzippedPayload /* = bytes of a gzip-compressed document */;
Router router;
router.setPath(http::Method::GET, "/data", [&gzippedPayload](const HttpRequestView&, HttpResponseWriter& writer) {
  writer.status(http::StatusCodeOK);
  writer.contentType("application/json");
  writer.contentEncoding("gzip");  // sent as is, never compressed again
  writer.writeBody(gzippedPayload);
  writer.end();
});
```

`Content-Encoding: identity` sends a body uncompressed, even above the threshold. The value is not validated, and the server does not add `Vary`: add it if caches may store the response. Check that the client accepts the coding you send.

## Request body decompression

When a codec is compiled in, request bodies with a `Content-Encoding` header are decoded before the handler runs: the handler sees the decoded body, and the `Content-Encoding` header is removed. Stacked codings (`Content-Encoding: gzip, zstd`) are decoded in reverse order, the last coding first, and `identity` is skipped.

Codings are case-insensitive: the server lower-cases the received `Content-Encoding` value when it parses the request (over HTTP/1.1 and HTTP/2), so a handler reads `gzip` for `GZip`, also when decompression is disabled.

Decompression is the classic vector of "compression bombs": a few kilobytes that expand to gigabytes. `DecompressionConfig`, passed to `HttpServerConfig::withRequestDecompression()`, bounds it:

```cpp
DecompressionConfig decompression;
decompression.maxCompressedBytes = 16U << 20U;    // 16 MiB of compressed input at most
decompression.maxDecompressedBytes = 64U << 20U;  // 64 MiB of decoded body at most
decompression.maxExpansionRatio = 100.0;          // and no more than 100 times the input

HttpServerConfig config;
config.withRequestDecompression(decompression);
```

| Setting | Default | Effect |
| --- | --- | --- |
| `enable` | `true` when a codec is compiled | `false` passes encoded bodies to handlers unchanged, with their `Content-Encoding`. |
| `maxCompressedBytes` | 128 MiB | Largest compressed body the server attempts to decode. |
| `maxDecompressedBytes` | 4 GiB | Largest decoded body. Lower it to what your handlers accept. |
| `maxExpansionRatio` | 1000 | Largest ratio of decoded to compressed size, checked after each coding of a stack. |
| `streamingDecompressionThresholdBytes` | 16 MiB | From this `Content-Length`, decode with streaming decoders instead of materializing each stage of a stack. |
| `decoderChunkSize` | 32 KiB | Granularity of the output buffer growth. |

| Request | Response |
| --- | --- |
| Unknown or not compiled coding | `415 Unsupported Media Type` |
| Empty coding in the list (`gzip,,deflate`) | `400 Bad Request` |
| Corrupt compressed data | `400 Bad Request` |
| A size or ratio limit exceeded | `413 Payload Too Large` |

The decoded body is limited by `maxDecompressedBytes`, not by `maxBodyBytes`, which applies to the body as received. Request bodies are decoded entirely before the handler can read them.

## Tests

- Response compression, negotiation, thresholds, streaming, and direct compression end to end: [tests/http-compression_test.cpp](../../tests/http-compression_test.cpp).
- `HEAD` responses carrying the headers of the compressed `GET` ones (`HttpCompression.HeadCarriesTheHeadersOfTheGet`, `HttpCompression.StreamingHeadCarriesTheHeadersOfTheGetWithoutContentLength`): [tests/http-compression_test.cpp](../../tests/http-compression_test.cpp); over HTTP/2: [tests/http2-core_test.cpp](../../tests/http2-core_test.cpp) and [tests/http-protocol-parity_test.cpp](../../tests/http-protocol-parity_test.cpp).
- Request decompression, stacked codings, limits, and errors: [tests/http-request-decompression_test.cpp](../../tests/http-request-decompression_test.cpp).
- Lower-casing of the received codings: [http-request-view_test.cpp](../../aeronet/http/test/http-request-view_test.cpp) (HTTP/1.1) and [hpack_test.cpp](../../aeronet/http2/test/hpack_test.cpp) (HTTP/2).
- `Accept-Encoding` parsing and selection: [accept-encoding-negotiation_test.cpp](../../aeronet/objects/test/accept-encoding-negotiation_test.cpp).
- Codecs: [zlib-encoder-decoder_test.cpp](../../aeronet/objects/test/zlib-encoder-decoder_test.cpp), [zstd-encoder-decoder_test.cpp](../../aeronet/objects/test/zstd-encoder-decoder_test.cpp), [brotli-encoder-decoder_test.cpp](../../aeronet/objects/test/brotli-encoder-decoder_test.cpp).
