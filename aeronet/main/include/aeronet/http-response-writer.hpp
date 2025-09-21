// Streaming response writer for Aeronet HTTP/1.1 (phase 1 skeleton)
#pragma once

#include <cstddef>
#include <string_view>

#include "flat-hash-map.hpp"
#include "http-status-code.hpp"
#include "string.hpp"

namespace aeronet {

class HttpServer;  // fwd

class HttpResponseWriter {
 public:
  HttpResponseWriter(HttpServer& srv, int fd, bool headRequest);

  void setStatus(http::StatusCode code, std::string_view reason = {});
  void setHeader(std::string_view name, std::string_view value);
  void setContentType(std::string_view ct) { setHeader("Content-Type", ct); }
  void setContentLength(std::size_t len);

  // Backpressure-aware write. Returns true if accepted (queued or immediately written). Returns
  // false if a fatal error occurred or the server marked the connection for closure / overflow.
  bool write(std::string_view data);
  void end();

  [[nodiscard]] bool finished() const { return _ended; }

 private:
  void ensureHeadersSent();
  void emitChunk(std::string_view data);
  void emitLastChunk();

  HttpServer* _server{nullptr};
  int _fd{-1};
  bool _head{false};
  bool _headersSent{false};
  bool _chunked{true};
  bool _ended{false};
  bool _failed{false};
  http::StatusCode _statusCode{200};
  string _reason{"OK"};
  flat_hash_map<string, string> _headers;
  std::size_t _declaredLength{0};
  std::size_t _bytesWritten{0};
  bool enqueue(const char* data, std::size_t len);
  bool enqueue(std::string_view sv) { return enqueue(sv.data(), sv.size()); }
};

}  // namespace aeronet
