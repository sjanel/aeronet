// Streaming response writer for Aeronet HTTP/1.1 (phase 1 skeleton)
#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/encoding.hpp"
#include "aeronet/http-response.hpp"
#include "encoder.hpp"
#include "http-constants.hpp"
#include "http-status-code.hpp"
#include "raw-chars.hpp"

namespace aeronet {

class HttpServer;

class HttpResponseWriter {
 public:
  HttpResponseWriter(HttpServer& srv, int fd, bool headRequest, bool requestConnClose, Encoding compressionFormat);

  void statusCode(http::StatusCode code, std::string_view reason = {});

  void header(std::string_view name, std::string_view value);

  void contentType(std::string_view ct) { header(http::ContentType, ct); }

  void contentLength(std::size_t len);

  // Backpressure-aware write. Returns true if accepted (queued or immediately written). Returns
  // false if a fatal error occurred or the server marked the connection for closure / overflow.
  bool write(std::string_view data);
  void end();

  [[nodiscard]] bool finished() const { return _ended; }
  // Per-response opt-out: streaming writer path.

  void disableAutoCompression() { _disableAutoCompression = true; }

  [[nodiscard]] bool autoCompressionDisabled() const { return _disableAutoCompression; }

 private:
  void ensureHeadersSent();
  void emitChunk(std::string_view data);
  void emitLastChunk();
  bool enqueue(std::string_view data);

  HttpServer* _server{nullptr};
  int _fd{-1};
  bool _head{false};
  bool _headersSent{false};
  bool _chunked{true};
  bool _ended{false};
  bool _failed{false};
  bool _requestConnClose{false};
  bool _userSetContentType{false};
  bool _disableAutoCompression{false};
  Encoding _compressionFormat;
  bool _compressionActivated{false};
  // Internal fixed HttpResponse used solely for header accumulation and status/reason/body placeholder.
  // We never finalize until ensureHeadersSent(); body remains empty (streaming chunks / writes follow separately).
  HttpResponse _fixedResponse{200, http::ReasonOK};
  std::size_t _declaredLength{0};
  std::size_t _bytesWritten{0};
  RawChars _chunkBuf;                                 // reusable buffer for coalesced small/medium chunks
  std::unique_ptr<EncoderContext> _activeEncoderCtx;  // streaming context
  RawChars _preCompressBuffer;                        // threshold buffering before activation
};

}  // namespace aeronet
