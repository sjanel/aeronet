#pragma once

#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/decimal-writer.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/system-error-message.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/writer-transport.hpp"

namespace aeronet {

/// HTTP/1.1 transport backend for HttpResponseWriter.
/// Emits chunked transfer-encoding or fixed-length framed data over a TCP connection.
class Http1WriterTransport final : public IWriterTransport {
 public:
  Http1WriterTransport(internal::ServerContext& ctx, NativeHandle fd, const CorsPolicy* pCorsPolicy,
                       std::span<const ResponseMiddleware> routeResponseMiddleware)
      : _ctx(ctx), _fd(fd), _pCorsPolicy(pCorsPolicy), _routeResponseMiddleware(routeResponseMiddleware) {}

  bool emitHeaders(HttpResponse& response, const HttpRequestView& request, std::size_t declaredLength,
                   bool isHead) override {
    const bool chunked = !isHead && (declaredLength == 0) && !response.hasBodyFile();

    // The head announces the close of the connection after this response, whatever its reason (RFC 9112 §9.6). It is
    // decided when the head is sent, so that it also announces a drain that began while the handler ran.
    if (!_ctx.http1.isKeepAlive(request)) {
      response._opts.setClose();
      _closeAnnounced = true;
    }

    // Transfer-Encoding or Content-Length header
    if (chunked) {
      const std::size_t needed = http::HeaderSize(http::TransferEncoding.size(), http::chunked.size());

      response.reserve(response.sizeInlined() + needed);
      response.headerAddLineUnchecked(http::TransferEncoding, http::chunked);
      _chunked = true;
    } else if (!response.hasBodyFile()) {
      const auto declaredLengthIntegralLen = ndigits(declaredLength);
      const std::size_t needed = http::HeaderSize(http::ContentLength.size(), declaredLengthIntegralLen);

      response.reserve(response.sizeInlined() + needed);
      char declaredLenBuf[std::numeric_limits<decltype(declaredLength)>::digits10 + 1];
      std::string_view declaredLenStr(declaredLenBuf,
                                      WriteUInt(declaredLenBuf, declaredLength, declaredLengthIntegralLen));
      response.headerAddLineUnchecked(http::ContentLength, declaredLenStr);
      _chunked = false;
    }

    _ctx.requestPipeline().prepareStreamingHead(request, response, _routeResponseMiddleware, _pCorsPolicy);

    // The body is streamed separately (chunked framing or a fixed Content-Length was declared above), so the
    // buffer finalized here is header-only: tell finalizeForHttp1 not to synthesize a Content-Length: 0.
    response._opts.setStreamingBody();

    auto cnxIt = _ctx.connections.iterator(_fd);
    _ctx.io.queueData(cnxIt, response.finalizeForHttp1(_ctx.dateHeader.data(), http::HTTP_1_1, response._opts, nullptr,
                                                       _ctx.config.minCapturedBodySize));
    ConnectionState& cnx = _ctx.connections.connectionState(cnxIt);
    if (cnx.isAnyCloseRequested()) {
      log::error("Http1WriterTransport: failed to enqueue headers fd # {} err={} msg={}", _fd, LastSystemError(),
                 SystemErrorMessage(LastSystemError()));
      return false;
    }
    return true;
  }

  bool emitData(std::string_view data) override {
    assert(!data.empty());

    if (_chunked) {
      // Format: hex-size\r\ndata\r\n
      const auto nbDigitsHex = hex_digits(data.size());
      const auto totalSize = nbDigitsHex + http::CRLF.size() + data.size() + http::CRLF.size();

      RawChars chunkBuffer(totalSize);
      char* pData = to_lower_hex(data.size(), chunkBuffer.data());

      pData = AppendFixed<http::CRLF>(pData);

      pData = Append(data, pData);

      pData = AppendFixed<http::CRLF>(pData);
      chunkBuffer.setSize(totalSize);

      return enqueue(HttpMessageData(std::move(chunkBuffer)));
    }
    return enqueue(HttpMessageData(RawChars(data)));
  }

  bool emitEnd(RawChars trailers) override {
    if (!_chunked) {
      return true;  // Fixed-length: no end marker needed
    }

    // Emit final chunk with optional trailers (RFC 7230 §4.1.2):
    //   0\r\n
    //   [trailer-name: value\r\n]*
    //   \r\n
    trailers.ensureAvailableCapacity(http::EndChunk.size());
    if (trailers.empty()) {
      trailers.unchecked_append(http::EndChunk);
    } else {
      // trailers already contain header lines; prepend "0\r\n" and append final "\r\n"
      // shift right to make space for "0\r\n"
      std::memmove(trailers.data() + 1UL + http::CRLF.size(), trailers.data(), trailers.size());

      static constexpr std::string_view kEndChunkPrefix = "0\r\n";

      char* pData = trailers.data();

      pData = AppendFixed<kEndChunkPrefix>(pData);
      pData += trailers.size();
      pData = AppendFixed<http::CRLF>(pData);

      trailers.setEnd(pData);
    }

    return enqueue(HttpMessageData(std::move(trailers)));
  }

  [[nodiscard]] uint32_t logId() const override { return static_cast<uint32_t>(_fd); }

  // Whether the head of the response announced the close of the connection after it.
  [[nodiscard]] bool closeAnnounced() const noexcept { return _closeAnnounced; }

 private:
  bool enqueue(HttpMessageData responseData) {
    auto cnxIt = _ctx.connections.iterator(_fd);
#ifdef AERONET_WINDOWS
    if (cnxIt == _ctx.connections.end()) {
#else
    if (!*cnxIt) {
#endif
      return false;
    }
    _ctx.io.queueData(cnxIt, std::move(responseData));
    ConnectionState& state = _ctx.connections.connectionState(cnxIt);
    return !state.isAnyCloseRequested();
  }

  internal::ServerContext& _ctx;
  NativeHandle _fd;
  bool _closeAnnounced{false};
  bool _chunked{true};
  const CorsPolicy* _pCorsPolicy;
  std::span<const ResponseMiddleware> _routeResponseMiddleware;
};

}  // namespace aeronet
