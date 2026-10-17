#include "aeronet/internal/response-streamer.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string_view>

#include "aeronet/decompression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-client-codec.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/log.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/response-body-sink.hpp"

#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-decoder.hpp"
#endif
#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-decoder.hpp"
#include "aeronet/zlib-stream-raii.hpp"
#endif
#ifdef AERONET_ENABLE_ZSTD
#include "aeronet/zstd-decoder.hpp"
#endif

namespace aeronet::internal {

namespace {

// Whether `coding` (a single token of a Content-Encoding value) can be decoded by this build.
bool IsSupportedCoding(std::string_view coding) noexcept {
  return coding == http::identity
#ifdef AERONET_ENABLE_ZLIB
         || coding == http::gzip || coding == http::deflate
#endif
#ifdef AERONET_ENABLE_ZSTD
         || coding == http::zstd
#endif
#ifdef AERONET_ENABLE_BROTLI
         || coding == http::br
#endif
      ;
}

}  // namespace

void ResponseStreamer::beginRequest(ResponseBodySink* sink, const HttpClientConfig& config,
                                    HttpClientCodec& codec) noexcept {
  _sink = sink;
  _headDelivered = false;
  if (sink != nullptr) {
    _config = &config;
    _codec = &codec;
  }
}

void ResponseStreamer::beginExchange(bool redirectFollowed, bool statusRetried) noexcept {
  _decoder = nullptr;
  _compressedBytes = 0;
  _decodedBytes = 0;
  _error = HttpClientErrc::noError;
  _encoding = Encoding::none;
  _bufferedBody = {};
  _bufferRedirects = redirectFollowed;
  _bufferRetries = statusRetried;
}

ResponseStreamer::Coding ResponseStreamer::selectDecoder(std::string_view contentEncoding) {
  // Codings are compared exactly: the response parsers lower-case received Content-Encoding values.
  assert(!LowerAsciiKey::ContainsUpperAscii(contentEncoding));
  if (contentEncoding.contains(',')) {
    // Stacked codings are decoded whole, once buffered: they must all be supported.
    for (http::HeaderValueReverseTokensIterator<','> it(contentEncoding); it.hasNext();) {
      if (!IsSupportedCoding(it.next())) {
        return Coding::Unsupported;
      }
    }
    return Coding::Buffered;
  }
  if (contentEncoding == http::identity) {
    return Coding::Streamed;
  }
#ifdef AERONET_ENABLE_ZLIB
  const bool isGzip = contentEncoding == http::gzip;
  if (isGzip || contentEncoding == http::deflate) {
    ZlibDecoder& decoder = _codec->decompressionState.zlibDecoder;
    decoder.setVariant(isGzip ? ZStreamRAII::Variant::gzip : ZStreamRAII::Variant::deflate);
    _decoder = decoder.makeContext();
    _encoding = isGzip ? Encoding::gzip : Encoding::deflate;
    return Coding::Streamed;
  }
#endif
#ifdef AERONET_ENABLE_ZSTD
  if (contentEncoding == http::zstd) {
    _decoder = _codec->decompressionState.zstdDecoder.makeContext();
    _encoding = Encoding::zstd;
    return Coding::Streamed;
  }
#endif
#ifdef AERONET_ENABLE_BROTLI
  if (contentEncoding == http::br) {
    _decoder = _codec->decompressionState.brotliDecoder.makeContext();
    _encoding = Encoding::br;
    return Coding::Streamed;
  }
#endif
  return Coding::Unsupported;
}

HttpClientErrc ResponseStreamer::startResponse(HttpResponse& head, ResponseBodyInfo body, bool& stream) {
  assert(active());
  stream = false;
  const http::StatusCode status = head.status();
  if ((_bufferRedirects && IsRedirectStatus(status) && !head.headerValueOrEmpty(http::Location).empty()) ||
      (_bufferRetries && _config->retry.retryStatuses.contains(status))) {
    return HttpClientErrc::noError;  // this response will be followed or retried, not delivered
  }
  const std::string_view contentEncoding = head.headerValueOrEmpty(http::ContentEncoding);
  if (!contentEncoding.empty() && _config->decompression.enable) {
    switch (selectDecoder(contentEncoding)) {
      case Coding::Buffered:
        return HttpClientErrc::noError;
      case Coding::Unsupported:
        // Rejected like a buffered body would be, without receiving it first.
        log::error("HTTP client: unsupported Content-Encoding '{}' of a streamed response", contentEncoding);
        _error = HttpClientErrc::malformedResponse;
        return _error;
      default:
        break;
    }
    if (_encoding != Encoding::none) {
      body.size = ResponseBodyInfo::kUnknownSize;  // the Content-Length counts the coded bytes
    }
    head.headerRemoveLine(http::ContentEncoding);  // the sink receives the decoded body
  }
  if (body.contentType.empty()) {
    body.contentType = http::ContentTypeApplicationOctetStream;
  }
  stream = true;
  _headDelivered = true;
  if (!_sink->onHead(head, body)) {
    log::debug("HTTP client: transfer aborted by the response body sink at the head");
    _error = HttpClientErrc::aborted;
  }
  return _error;
}

bool ResponseStreamer::deliver(std::string_view data) {
  if (!_sink->onBody(data)) {
    log::debug("HTTP client: transfer aborted by the response body sink");
    _error = HttpClientErrc::aborted;
    return false;
  }
  return true;
}

bool ResponseStreamer::decode(std::string_view chunk, bool finalChunk) {
  const DecompressionConfig& cfg = _config->decompression;
  RawChars& out = _codec->decompressOut;
  // A zero limit means none for the decoders: always leave them at least one byte of budget.
  // Unused when no decoder is compiled in.
  [[maybe_unused]] const std::size_t budget = std::max<std::size_t>(cfg.maxDecompressedBytes - _decodedBytes, 1);
  const std::size_t sizeBefore = out.size();
  bool decoded = false;
  switch (_encoding) {
#ifdef AERONET_ENABLE_ZLIB
    case Encoding::gzip:
      [[fallthrough]];
    case Encoding::deflate:
      decoded = static_cast<ZlibDecoderContext*>(_decoder)->decompressChunk(chunk, finalChunk, budget,
                                                                            cfg.decoderChunkSize, out);
      break;
#endif
#ifdef AERONET_ENABLE_ZSTD
    case Encoding::zstd:
      decoded = static_cast<ZstdDecoderContext*>(_decoder)->decompressChunk(chunk, finalChunk, budget,
                                                                            cfg.decoderChunkSize, out);
      break;
#endif
    default:
#ifdef AERONET_ENABLE_BROTLI
      // selectDecoder() only selects the compiled-in codings, and none is never decoded.
      assert(_encoding == Encoding::br);
      decoded = static_cast<BrotliDecoderContext*>(_decoder)->decompressChunk(chunk, finalChunk, budget,
                                                                              cfg.decoderChunkSize, out);
#endif
      break;
  }
  _compressedBytes += chunk.size();
  _decodedBytes += out.size() - sizeBefore;
  // The expansion ratio is that of the whole body, as for a buffered one. While the body arrives, it is checked
  // against at least one decoder chunk of input, so that a very compressible start (zeroes) does not fail a body whose
  // overall ratio is fine.
  const std::size_t ratioBase =
      finalChunk ? _compressedBytes : std::max<std::size_t>(_compressedBytes, cfg.decoderChunkSize);
  // The decoders fail on corrupt input as when reaching their budget: being within one decoder chunk of the limit
  // tells the latter (like for a buffered body). An empty chunk decodes nothing, so it can only fail on a truncated
  // stream.
  if (_decodedBytes > cfg.maxDecompressedBytes ||
      static_cast<double>(_decodedBytes) > static_cast<double>(ratioBase) * cfg.maxExpansionRatio ||
      (!decoded && !chunk.empty() && _decodedBytes + cfg.decoderChunkSize > cfg.maxDecompressedBytes)) {
    log::error("HTTP client: streamed {} response body exceeds the decompression limits ({} bytes decoded from {})",
               GetEncodingStr(_encoding), _decodedBytes, _compressedBytes);
    _error = HttpClientErrc::responseTooLarge;
    return false;
  }
  if (!decoded) {
    log::error("HTTP client: cannot decode the streamed {} response body (after {} bytes decoded from {})",
               GetEncodingStr(_encoding), _decodedBytes, _compressedBytes);
    _error = HttpClientErrc::malformedResponse;
    return false;
  }
  return true;
}

bool ResponseStreamer::feed(std::string_view data) {
  assert(!data.empty());
  if (_encoding == Encoding::none) {
    return deliver(data);
  }
  RawChars& out = _codec->decompressOut;
  out.clear();
  return decode(data, false) && (out.empty() || deliver(out));
}

bool ResponseStreamer::finish() {
  // Nothing to check for an identity body, or an empty content-coded one. Otherwise, all the received bytes are
  // decoded already: an empty final chunk checks that they complete the coded stream, and the expansion ratio of the
  // whole body.
  return _encoding == Encoding::none || _compressedBytes == 0 || decode({}, true);
}

void ResponseStreamer::setBufferedBody(std::string_view body, std::string_view contentType) {
  _bufferedBody = body;
  _bufferedContentType.assign(contentType);
}

HttpClientErrc ResponseStreamer::deliverBuffered(const HttpResponse& head) {
  assert(active() && !_headDelivered);
  _headDelivered = true;
  if (_bufferedBody.empty()) {
    return _sink->onHead(head, ResponseBodyInfo{.contentType = {}, .size = 0}) ? HttpClientErrc::noError
                                                                               : HttpClientErrc::aborted;
  }
  // Only bodyless responses, redirects that are not followed and bodies with stacked codings are delivered this way.
  const std::string_view contentType =
      _bufferedContentType.empty() ? http::ContentTypeApplicationOctetStream : std::string_view(_bufferedContentType);
  if (!_sink->onHead(head, ResponseBodyInfo{.contentType = contentType, .size = _bufferedBody.size()}) ||
      !_sink->onBody(_bufferedBody)) {
    return HttpClientErrc::aborted;
  }
  return HttpClientErrc::noError;
}

}  // namespace aeronet::internal
