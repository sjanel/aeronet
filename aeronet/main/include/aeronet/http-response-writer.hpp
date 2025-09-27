// Streaming response writer for Aeronet HTTP/1.1 (phase 1 skeleton)
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "flat-hash-map.hpp"
#include "http-constants.hpp"
#include "http-status-code.hpp"

namespace aeronet {

class HttpServer;  // fwd

class HttpResponseWriter {
 public:
  HttpResponseWriter(HttpServer& srv, int fd, bool headRequest, bool requestConnClose);

  void statusCode(http::StatusCode code, std::string reason = {});

  void header(std::string name, std::string value);

  void contentType(std::string ct) { header(std::string(http::ContentType), std::move(ct)); }

  void contentLength(std::size_t len);

  // Backpressure-aware write. Returns true if accepted (queued or immediately written). Returns
  // false if a fatal error occurred or the server marked the connection for closure / overflow.
  bool write(std::string_view data);
  void end();

  [[nodiscard]] bool finished() const { return _ended; }

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
  http::StatusCode _statusCode{200};
  std::string _reason{"OK"};
  flat_hash_map<std::string, std::string, std::hash<std::string_view>, std::equal_to<>> _headers;
  std::size_t _declaredLength{0};
  std::size_t _bytesWritten{0};
};

}  // namespace aeronet
