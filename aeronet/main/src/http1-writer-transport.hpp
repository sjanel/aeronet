#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/system-error-message.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/writer-transport.hpp"

namespace aeronet::internal {

/// HTTP/1.1 transport backend for HttpResponseWriter.
/// Emits chunked transfer-encoding or fixed-length framed data over a TCP connection.
class Http1WriterTransport final : public IWriterTransport {
 public:
  Http1WriterTransport(SingleHttpServer& server, NativeHandle fd, bool requestConnClose, const CorsPolicy* pCorsPolicy,
                       std::span<const ResponseMiddleware> routeResponseMiddleware)
      : _server(&server),
        _fd(fd),
        _requestConnClose(requestConnClose),
        _pCorsPolicy(pCorsPolicy),
        _routeResponseMiddleware(routeResponseMiddleware) {}

  bool emitHeaders(HttpResponse& response, const HttpRequest& request, [[maybe_unused]] bool compressionActivated,
                   [[maybe_unused]] Encoding compressionFormat, std::size_t declaredLength, bool isHead) override {
    const bool chunked = !isHead && (declaredLength == 0) && !response.hasBodyFile();

    // Add Connection: close if requested
    if (_requestConnClose && !_connCloseEmitted) {
      response.headerAddLineUnchecked(http::Connection, http::close);
      response._opts.close(false);
      _connCloseEmitted = true;
    }

    // Transfer-Encoding or Content-Length header
    if (chunked) {
      const std::size_t needed = HttpResponse::HeaderSize(http::TransferEncoding.size(), http::chunked.size());
      response._data.ensureAvailableCapacity(needed);
      response.headerAddLineUnchecked(http::TransferEncoding, http::chunked);
      _chunked = true;
    } else if (!response.hasBodyFile()) {
      const std::size_t needed = HttpResponse::HeaderSize(http::ContentLength.size(), nchars(declaredLength));
      response._data.ensureAvailableCapacity(needed);
      response.headerAddLineUnchecked(http::ContentLength, std::string_view(IntegralToCharVector(declaredLength)));
      _chunked = false;
    }

    ApplyResponseMiddleware(request, response, _routeResponseMiddleware, _server->_router.globalResponseMiddleware(),
                            _server->_telemetry, true, _server->_callbacks.middlewareMetrics);

    if (_pCorsPolicy != nullptr) {
      (void)_pCorsPolicy->applyToResponse(request, response);
      _pCorsPolicy = nullptr;
    }

    auto cnxIt = _server->_connections.iterator(_fd);
    _server->queueData(cnxIt, response.finalizeForHttp1(SysClock::now(), http::HTTP_1_1, response._opts, nullptr,
                                                        _server->config().minCapturedBodySize));
    ConnectionState& cnx = _server->_connections.connectionState(cnxIt);
    if (cnx.isAnyCloseRequested()) {
      log::error("Http1WriterTransport: failed to enqueue headers fd # {} err={} msg={}", _fd, LastSystemError(),
                 SystemErrorMessage(LastSystemError()));
      return false;
    }
    return true;
  }

  bool emitData(std::string_view data) override {
    if (data.empty()) {
      return true;
    }

    HttpResponseData responseData;

    if (_chunked) {
      // Format: hex-size\r\ndata\r\n
      const auto nbDigitsHex = hex_digits(data.size());
      const auto totalSize = nbDigitsHex + http::CRLF.size() + data.size() + http::CRLF.size();

      RawChars chunkBuffer(totalSize);
      char* insertPtr = to_lower_hex(data.size(), chunkBuffer.data());
      insertPtr = Append(http::CRLF, insertPtr);
      insertPtr = Append(data, insertPtr);
      insertPtr = Append(http::CRLF, insertPtr);
      chunkBuffer.setSize(totalSize);

      responseData = HttpResponseData(std::move(chunkBuffer));
    } else {
      responseData = HttpResponseData(RawChars(data));
    }

    return enqueue(std::move(responseData));
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
      char* insertPtr = trailers.data();
      *insertPtr++ = '0';
      insertPtr = Append(http::CRLF, insertPtr);
      insertPtr += trailers.size();
      insertPtr = Append(http::CRLF, insertPtr);

      trailers.setSize(static_cast<std::size_t>(insertPtr - trailers.data()));
    }

    return enqueue(HttpResponseData(std::move(trailers)));
  }

  [[nodiscard]] bool isAlive() const override {
    auto cnxIt = _server->_connections.iterator(_fd);
    if (cnxIt == _server->_connections.end()) {
      return false;
    }
    return !_server->_connections.connectionState(cnxIt).isAnyCloseRequested();
  }

  [[nodiscard]] uint32_t logId() const override { return static_cast<uint32_t>(_fd); }

 private:
  bool enqueue(HttpResponseData responseData) {
    auto cnxIt = _server->_connections.iterator(_fd);
    if (cnxIt == _server->_connections.end()) {
      return false;
    }
    _server->queueData(cnxIt, std::move(responseData));
    ConnectionState& state = _server->_connections.connectionState(cnxIt);
    return !state.isAnyCloseRequested();
  }

  SingleHttpServer* _server;
  NativeHandle _fd;
  bool _requestConnClose;
  bool _connCloseEmitted{false};
  bool _chunked{true};
  const CorsPolicy* _pCorsPolicy;
  std::span<const ResponseMiddleware> _routeResponseMiddleware;
};

}  // namespace aeronet::internal
