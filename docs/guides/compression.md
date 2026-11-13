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
4. sets `Content-Encoding`, updates `Content-Length`, and adds `Accept-Encoding` to `Vary` (unless the `Vary` of the handler lists it already, or is `*`), so that caches keep one version per coding.

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
| `maxCompressRatio` | 0.6 | The compressed body is kept only if its size is at most this fraction of the original (at least 40% smaller by default). The compression stops as soon as its output exceeds this size. |
| `contentTypeAllowList` | empty: every type | Media types eligible for compression, compared case-insensitively and without their parameters: `text/html` matches `Content-Type: text/html; charset=utf-8`. |
| `preferredFormats` | empty | Codings preferred by the server, used to break ties (see below). |
| `addVaryAcceptEncodingHeader` | `true` | Add `Accept-Encoding` to the `Vary` of compressed responses. |
| `addVaryAcceptEncodingToUncompressed` | `false` | With `addVaryAcceptEncodingHeader`, also add it to the uncompressed responses that would be compressed for a client accepting a coding (see [Vary](#vary)). |
| `defaultDirectCompressionMode` | `Auto` | See [Direct compression](#direct-compression). |
| `initialCompressionBufferLimit` | 32 KiB | Output buffer reserved upfront; larger bodies grow it while compressing, up to `maxCompressRatio` of the body size. |
| `useLeadingZeroesInContentLength` | `true` | A body compressed when the response is finalized gets a `Content-Length` as wide as the largest accepted compressed size (`maxCompressRatio` of the body size), padded with leading zeroes (`content-length: 0333`), which avoids moving the body. [Direct compression](#direct-compression) writes the exact length, and streamed responses have none. Leading zeroes are valid HTTP (`1*DIGIT`), but a parser reading `010` as octal 8 disagrees with aeronet on where the body ends, which a proxy chain can turn into request smuggling: set `false` when such a parser may sit between the server and its clients. Also applies to the request bodies compressed by `HttpClient` (`requestCompression.codec`). |
| `zlib.level`, `zstd.compressionLevel`, `zstd.windowLog`, `brotli.quality`, `brotli.window` | library defaults | Codec tuning: higher levels compress better and cost more CPU. |

Already compressed content, such as images, video, or archives, gains nothing: restrict `contentTypeAllowList` to text types (HTML, JSON, JavaScript, CSS, SVG) when a server mixes both, to save the CPU of a compression attempt that would be discarded.

### Negotiation

The `Accept-Encoding` header lists the codings a client accepts, with optional quality values (`q`, from 0 to 1 with up to 3 decimals, default 1). A quality outside this grammar (RFC 9110 §12.4.2), such as `q=2`, `q=0.0001` or `q=inf`, counts as 0 for a coding. On `identity` or `*`, such an element is ignored instead: a malformed weight does not indicate that the uncompressed form is unacceptable, which would answer every request of the client `406` (RFC 9110 §12.5.3). The server picks the compiled coding with the highest quality; `gzip;q=0.5, br;q=0.9` selects `br`. A quality of 0 excludes a coding, and `*` stands for every coding not listed. A coding or a `*` listed several times keeps its highest quality, wherever its elements are: `*;q=0, *` and `*, *;q=0` select the same coding.

When several codings share the best quality, the server preference decides: the codings of `preferredFormats` in their order, then the other compiled codings in the default order, `zstd`, `br`, `gzip`, `deflate`, and finally the uncompressed form (`identity`). With `preferredFormats` set to `gzip` only, `Accept-Encoding: br, gzip` selects `gzip`, while `Accept-Encoding: br` still selects `br`.

Without `Accept-Encoding`, or when no compiled coding is acceptable, the response is sent uncompressed. If the client also excludes the uncompressed form (`identity;q=0`, or `*;q=0` without `identity`), the server answers `406 Not Acceptable`, before calling any handler. Over HTTP/1.1, the connection stays open for the next requests, unless the request announces a body (`Transfer-Encoding`, or a `Content-Length` other than `0`): the body is not read, so the connection closes after the `406`. An invalid `Content-Length` is answered `400` instead (see [Content-Length](requests.md#content-length)).

Each event loop remembers the result for the last `Accept-Encoding` value it negotiated, if it is not longer than that of the current browsers (`gzip, deflate, br, zstd`, 23 bytes): clients mostly send the value of their browser or HTTP library, so that the negotiation of most requests is a comparison with the previous one.

### What is not compressed

- Bodies smaller than `minBytes`, of a content type outside `contentTypeAllowList`, or that do not shrink enough. Compact binary encodings often do not, such as small [Protocol Buffers](../modules/protobuf.md#compression) messages.
- Responses that already have a `Content-Encoding` header (see [Send your own encoding](#send-your-own-encoding)).
- `206 Partial Content` responses: their `Content-Range` describes the bytes of the content sent (RFC 9110 §14.4), which compression would change. This holds for buffered, directly compressed and streamed responses. As [direct compression](#direct-compression) compresses a body while it is set, set the `206` status before the body: `status(206)` on a body compressed this way throws `std::logic_error`.
- File bodies (`HttpResponse::file()`, the static file handler), which are sent with `sendfile()` or read from disk as they are. Store pre-compressed variants of large static assets if they need compression.

### Vary

A compressed response lists `Accept-Encoding` in its `Vary`, after the field names of the `Vary` of the handler (unless one of them is `Accept-Encoding` or `*`): a cache does not serve it to a client accepting no coding.

The same resource is sent uncompressed to the clients accepting no coding, and when its compression does not shrink it enough: that response is selected with `Accept-Encoding` too, and RFC 9110 §12.5.5 recommends that it lists it as well. By default it does not, as with nginx (`gzip_vary` off): a shared cache holding it may serve it to every client, which is correct, only not compressed, and the uncompressed responses save a 24-byte header line. Set `addVaryAcceptEncodingToUncompressed` to follow the recommendation: the server then adds `Accept-Encoding` to the `Vary` of every response that it would compress for a client accepting a coding - a body in memory of at least `minBytes` (any size for a [direct compression](#direct-compression) in `DirectCompressionMode::On`), of an allowed content type, without a `Content-Encoding` of the handler, and not a `206` - compressed or not. The responses never compressed (too small, files, other content types) get none.

```cpp
CompressionConfig compression;
compression.addVaryAcceptEncodingToUncompressed = true;  // Vary: Accept-Encoding on the uncompressed variants too
```

A response made with `HttpRequestView::makeResponse()` writes this line with its body headers, in its buffer, without moving it: reserving the room of the body with `makeResponse(HttpResponse::BodySize(n), status)` includes it. A streamed response without a declared length cannot know its final size when its head is sent: written by a body write, it varies.

### Entity tags

A strong `ETag` identifies the exact bytes of a representation, and a compressed representation has other bytes than the uncompressed one: it needs another strong tag (RFC 9110 §8.8.3.3). When the server compresses a response carrying a strong `ETag`, it makes it weak (`"v1"` becomes `W/"v1"`), whatever the path (compressed when finalized, directly, or streamed) and whether the handler set the `ETag` before or after the body. A weak `ETag` is kept as is. `If-None-Match` uses the weak comparison, so `W/"v1"` still matches `"v1"`: a handler answering `304 Not Modified` should compare the tags it receives this way, ignoring a `W/` prefix. Range requests (`If-Range`) need a strong tag, which `206` responses keep, as they are never compressed.

### HEAD requests

A `HEAD` response carries the headers of the `GET` response (RFC 9110 §9.3.2), and its `Content-Length` must be the length of the `GET` content (§8.6). When a coding is negotiated, the server therefore compresses the body of a `HEAD` response like the one of a `GET`, then drops it: the response gets the same `Content-Encoding`, `Vary` and `Content-Length`, including when the body does not shrink enough and is sent uncompressed. Without a negotiated coding, nothing is compressed, and a response made with `makeResponse()` only keeps the size of the body set by the handler.

A streaming handler answering a `HEAD` request takes the same decision as for a `GET` (`Content-Encoding` once `minBytes` are written, or from the length declared with `contentLength()`, whatever the handler writes), but the body is neither compressed nor sent. As the length of a chunked `GET` response is not known when its head is sent, the `HEAD` response has no `Content-Length`, unless the handler declared one with `contentLength()` for a body that is not compressed.

### Streaming responses

For a response written with `HttpResponseWriter`, the server holds the headers and buffers the body until it reaches `minBytes`. It then decides once: either it starts a streaming encoder, adds `Content-Encoding`, and compresses each chunk as it is written, reusing one output buffer for the whole response, or the response ends before the threshold and is sent uncompressed with accurate headers. A content type outside `contentTypeAllowList` when the threshold is reached is sent uncompressed as well, from the buffered bytes on.

A handler declaring the length of its body with `contentLength(n)` gets this decision from `n` with its first write, without buffering. Below `minBytes`, or for a content type outside `contentTypeAllowList`, the response is sent uncompressed with `Content-Length: n`. Otherwise, the length of the compressed body is not known when the head is sent, and `Content-Length` is the number of octets sent (RFC 9110 §8.6): the declared length is dropped, and the response is chunked, or delimited by the close of the connection for an HTTP/1.0 client.

The output of one write is sent at once. brotli (from quality 4) and zstd write theirs in bursts, brotli once for several input blocks and zstd once per block of 128 KiB, which can exceed the output buffer on incompressible data: the buffer then grows, and keeps that capacity for the next writes of the response.

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

Set the mode for all responses with `CompressionConfig::defaultDirectCompressionMode`, or for one response with `HttpResponse::directCompressionMode()`. The compressed body grows in the response buffer as the encoder writes it, whatever the size of its output bursts (see [Streaming responses](#streaming-responses)). Direct compression only applies to inline bodies: an owned body passed by value (`body(std::string)`, `body(std::vector<char>)`) is captured without copy and compressed when the response is finalized, and a file body is never compressed. When direct compression is active, the server manages `Content-Encoding` and `Vary` as the body changes, and the finalization step does not compress again.

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

When a codec is compiled in, request bodies with a `Content-Encoding` header are decoded before the handler runs: the handler sees the decoded body, and the `Content-Encoding` header is removed. Stacked codings (`Content-Encoding: gzip, zstd`) are decoded in reverse order, the last coding first, and `identity` is skipped, as are empty list elements (`gzip,,zstd`, RFC 9110 §5.6.1.2): an empty `Content-Encoding` leaves the body as received. `x-gzip` is decoded as `gzip` (RFC 9110 §8.4.1.3), also by `HttpClient`. Each coding is a decoding pass over the whole body: at most 5 codings are accepted, `identity` and empty elements included, as curl does for responses (CVE-2023-23916). A longer list is answered `415` without decoding anything.

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
| `maxCompressedBytes` | 128 MiB | Largest compressed body the server attempts to decode. A body whose codings are all `identity` is not bounded by it. |
| `maxDecompressedBytes` | 4 GiB | Largest decoded body. Lower it to what your handlers accept. |
| `maxExpansionRatio` | 1000 | Largest ratio of decoded to compressed size. Each coding of a stack may decode up to this ratio times the size of the body as received: the decoders stop as soon as they exceed it (or `maxDecompressedBytes`), so a bomb is refused before it is inflated. |
| `streamingDecompressionThresholdBytes` | 16 MiB | From this `Content-Length`, decode with streaming decoders instead of materializing each stage of a stack. |
| `decoderChunkSize` | 32 KiB | Granularity of the output buffer growth. |

| Request | Response |
| --- | --- |
| Unknown or not compiled coding, more than 5 codings | `415 Unsupported Media Type`, with an `Accept-Encoding` header listing the codings the server decodes (RFC 9110 §12.5.3), so that a client can tell a refused coding from a refused media type and retry with a listed one |
| Corrupt compressed data | `400 Bad Request` |
| A size or ratio limit exceeded | `413 Payload Too Large` |

The decoded body is limited by `maxDecompressedBytes`, not by `maxBodyBytes`, which applies to the body as received. Request bodies are decoded entirely before the handler can read them.

`maxBodyBytes` and `maxCompressedBytes` are checked while the body is received, not once it is complete: from its declared length when it has one (`Content-Length`, or the `content-length` of an HTTP/2 request), so that a body above them is answered `413` before it is sent, without `100 Continue`; else from the chunk size line (HTTP/1.1) or the DATA frame (HTTP/2) exceeding them.

## Tests

- Response compression, negotiation, thresholds, streaming, and direct compression end to end: [tests/http-compression_test.cpp](../../tests/http-compression_test.cpp).
- `Accept-Encoding` listed once in `Vary`, after the `Vary` of the handler, buffered, directly compressed and streamed (`HttpCompression.VaryListsAcceptEncodingOnce`): [tests/http-compression_test.cpp](../../tests/http-compression_test.cpp).
- With `addVaryAcceptEncodingToUncompressed`, the uncompressed variant of a compressed resource varying on `Accept-Encoding`, for every way of building the response, `GET` and `HEAD`, and a body not shrinking enough, and the responses never compressed not varying; without it, only the compressed responses varying (`HttpCompression.IdentityVariantOfACompressedResourceVariesToo`, `HttpCompression.IdentityVariantOfAnUncompressedResourceDoesNotVary`, `HttpCompression.UncompressedResponsesDoNotVaryByDefault`): [tests/http-compression_test.cpp](../../tests/http-compression_test.cpp), over HTTP/1.1 and HTTP/2 (`ProtocolParity.IdentityVariantVariesOnAcceptEncoding`): [tests/http-protocol-parity_test.cpp](../../tests/http-protocol-parity_test.cpp); the `Vary` line written with the body headers, without reallocating the buffer (`HttpResponseTest.IdentityVariant*`): [http-response_test.cpp](../../aeronet/http/test/http-response_test.cpp).
- `406` keeping the HTTP/1.1 connection open, unless the request announces a body (`HttpCompression.NotAcceptableKeepsConnectionAlive`, `HttpCompression.NotAcceptableWithBodyClosesConnection`): [tests/http-compression_test.cpp](../../tests/http-compression_test.cpp).
- `HEAD` responses carrying the headers of the compressed `GET` ones (`HttpCompression.HeadCarriesTheHeadersOfTheGet`, `HttpCompression.StreamingHeadCarriesTheHeadersOfTheGetWithoutContentLength`): [tests/http-compression_test.cpp](../../tests/http-compression_test.cpp); over HTTP/2: [tests/http2-core_test.cpp](../../tests/http2-core_test.cpp) and [tests/http-protocol-parity_test.cpp](../../tests/http-protocol-parity_test.cpp).
- Streaming handlers declaring their length with `contentLength()`: kept below `minBytes` and for content types not compressed, dropped for a chunked compressed body, over HTTP/1.1 and HTTP/1.0, on a persistent connection, for `HEAD` (`HttpCompression.StreamedDeclaredLength*`, `HttpCompression.StreamedContentLengthAfterABufferedWriteIsIgnored`): [tests/http-compression_test.cpp](../../tests/http-compression_test.cpp).
- Request decompression, stacked codings, limits, and errors: [tests/http-request-decompression_test.cpp](../../tests/http-request-decompression_test.cpp), at most 5 codings (`HttpRequestDecompression.AtMostFiveStackedCodings`), empty elements ignored and `x-gzip` (`HttpRequestDecompression.EmptyElementsAreIgnored`, `HttpRequestDecompression.XGzipIsDecodedAsGzip`, `HttpCodecFullBody.XGzipIsGzip`, `HttpCodecDecompression.WillDecompressIgnoresEmptyElements` in [http-codec_test.cpp](../../aeronet/http/test/http-codec_test.cpp)), by `HttpClient` (`HttpClientSinkE2ETest.XGzipAndEmptyCodingElementsAreDecoded` in [http-client-core_test.cpp](../../aeronet/client/test/http-client-core_test.cpp)).
- `415` listing the decoded codings in `Accept-Encoding`, for an unknown coding, one in a stack, and more than 5 codings, with a `Content-Length` and chunked, and not for a corrupt body (`HttpRequestDecompression.UnsupportedCodingAnswerListsTheSupportedOnes`): [tests/http-request-decompression_test.cpp](../../tests/http-request-decompression_test.cpp), over HTTP/1.1 and HTTP/2 (`ProtocolParity.UnsupportedContentCodingAnswerListsTheDecodedOnes`): [tests/http-protocol-parity_test.cpp](../../tests/http-protocol-parity_test.cpp); the error response (`HttpErrorBuildTest.UnsupportedMediaTypeListsTheDecodedCodings`): [http-error-build_test.cpp](../../aeronet/server/test/http-error-build_test.cpp).
- `maxCompressedBytes` refusing a body from its `Content-Length` before it is received, also with `Expect: 100-continue`, and not bounding an `identity` body (`HttpRequestDecompression.MaxCompressedBytesOfAContentLengthBodyIsCheckedFromTheHead`): [tests/http-request-decompression_test.cpp](../../tests/http-request-decompression_test.cpp); over HTTP/2, from the `content-length` or the DATA frame exceeding it (`Http2ProtocolHandler.MaxCompressedBytesRefusesABodyToDecompressBeforeItIsReceived`): [http2-protocol-handler_test.cpp](../../aeronet/server/test/http2-protocol-handler_test.cpp).
- `206` responses never compressed and strong `ETag` made weak, on every compression path (`HttpCompression.PartialContentIsNotCompressed`, `HttpCompression.CompressionWeakensAStrongETag`): [tests/http-compression_test.cpp](../../tests/http-compression_test.cpp), over HTTP/1.1 and HTTP/2 (`ProtocolParity.CompressionKeeps206AndWeakensStrongETags`): [tests/http-protocol-parity_test.cpp](../../tests/http-protocol-parity_test.cpp); the `ETag` next to a `Vary` updated in the same pass (`HttpCodecCompression.CompressionWeakensAStrongETag`): [http-codec_test.cpp](../../aeronet/http/test/http-codec_test.cpp); `PartialContentIsNeverCompressedDirectly`, `WeakenStrongETag`: [http-response_test.cpp](../../aeronet/http/test/http-response_test.cpp).
- `maxExpansionRatio` stopping the decoding of a bomb before it is inflated, in one shot and streaming decoding, for a chunked body, and its exact boundary (`HttpCodecFullBody.ExpansionRatio*`, `HttpCodecDecompression.DecompressChunkedBody_ExpansionRatioStopsTheDecodingEarly`), stacked codings bound (`*.AtMostFiveStackedCodings`): [http-codec_test.cpp](../../aeronet/http/test/http-codec_test.cpp).
- Lower-casing of the received codings: [http-request-view_test.cpp](../../aeronet/http/test/http-request-view_test.cpp) (HTTP/1.1) and [hpack_test.cpp](../../aeronet/http2/test/hpack_test.cpp) (HTTP/2).
- `Accept-Encoding` parsing and selection: [accept-encoding-negotiation_test.cpp](../../aeronet/objects/test/accept-encoding-negotiation_test.cpp), an invalid weight of `identity` or `*` not answered `406` (`AcceptEncodingNegotiationTest.InvalidWeightOfIdentityOrWildcardIsNotARejection`), duplicate codings or wildcards not depending on their order (`AcceptEncodingNegotiationTest.AuditDuplicateWildcardDoesNotDependOnOrder`), quality values: [quality-value_test.cpp](../../aeronet/objects/test/quality-value_test.cpp).
- Bodies of a few compression chunks, and a compressed body filling the `maxCompressRatio` budget exactly or exceeding it by one byte (`HttpCodecCompression.TryCompressBodyOfAFewChunksUsesTheWholeBudget`, `HttpCodecCompression.TryCompressBodyStreamingBudgetBoundary`): [http-codec_test.cpp](../../aeronet/http/test/http-codec_test.cpp); request bodies compressed by `HttpClient` (`HttpClientCompressionE2E.CompressesMidSizeModeratelyCompressibleRequestBody`): [http-client-core_test.cpp](../../aeronet/client/test/http-client-core_test.cpp).
- Large incompressible bodies, whose brotli and zstd output comes in bursts larger than the room of one write, streamed, set with `bodyAppend()` and with `bodyInlineAppend()`, over HTTP/1.1 and HTTP/2 (`ProtocolParityFastBrotli.LargeIncompressibleBodiesAreCompressedWhole`): [tests/http-protocol-parity_test.cpp](../../tests/http-protocol-parity_test.cpp); request bodies directly compressed by `HttpClient` (`HttpClientCompressionE2E.LargeIncompressibleDirectlyCompressedRequestBody`): [http-client-core_test.cpp](../../aeronet/client/test/http-client-core_test.cpp), and restored after a const finalization (`HttpRequestTest.LargeIncompressibleDirectlyCompressedBody`): [http-request_test.cpp](../../aeronet/client/test/http-request_test.cpp); encoders reading part of their input only (`BrotliEncoderDecoderTest.IncompressibleChunksWithMinEncodeChunkCapacity`, `ZstdEncoderDecoderTest.LargeIncompressibleChunkWithMinEncodeChunkCapacity`, `*.EncodeChunkWithoutEnoughOutputRoom`).
- Codecs: [zlib-encoder-decoder_test.cpp](../../aeronet/objects/test/zlib-encoder-decoder_test.cpp), [zstd-encoder-decoder_test.cpp](../../aeronet/objects/test/zstd-encoder-decoder_test.cpp), [brotli-encoder-decoder_test.cpp](../../aeronet/objects/test/brotli-encoder-decoder_test.cpp).
