#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/http-status-code.hpp"
#include "socket.hpp"
#include "timedef.hpp"

namespace aeronet::test {
using namespace std::chrono_literals;

class ClientConnection {
 public:
  ClientConnection() noexcept = default;

  explicit ClientConnection(uint16_t port, std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

  [[nodiscard]] int fd() const noexcept { return _socket.fd(); }

 private:
  ::aeronet::Socket _socket;
};

// Minimal parsed HTTP response representation for test assertions.
struct ParsedResponse {
  aeronet::http::StatusCode statusCode{0};
  bool chunked{false};
  std::string reason;
  std::string headersRaw;                      // raw header block including final CRLFCRLF (optional)
  std::map<std::string, std::string> headers;  // case-sensitive keys (sufficient for tests)
  std::string body;                            // decoded body (if chunked, de-chunked)
  std::string plainBody;                       // de-chunked payload (available if Transfer-Encoding: chunked
};

// Result for content-encoding/body extraction helper used by compression tests.
struct EncodingAndBody {
  std::string_view contentEncoding;  // empty if absent
  std::string body;                  // de-chunked raw body (compressed bytes if encoded)
};

// From a raw HTTP response (headers + body), return the Content-Encoding header value
// and the de-chunked raw body. The returned body is not decompressed; callers should
// apply the appropriate decoder based on contentEncoding.
EncodingAndBody extractContentEncodingAndBody(std::string_view raw);

struct RequestOptions {
  std::string method{"GET"};
  std::string target{"/"};
  std::string host{"localhost"};
  std::string connection{"close"};
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;  // additional headers
  int recvTimeoutSeconds{2};                                 // socket receive timeout
  std::size_t maxResponseBytes{1 << 20};                     // 1 MiB safety cap
};

bool sendAll(int fd, std::string_view data, std::chrono::milliseconds totalTimeout = 500ms);

// Reads until we have a complete HTTP response or timeout.
// For chunked responses, continues reading until the terminating chunk (0\r\n\r\n).
// For responses with Content-Length, continues until body is complete.
// For Connection: close responses, reads until peer closes or timeout.
std::string recvWithTimeout(int fd, std::chrono::milliseconds totalTimeout = 2000ms);

std::string recvUntilClosed(int fd);

std::string sendAndCollect(uint16_t port, std::string_view raw);

// Start a simple echo server bound to loopback on an ephemeral port. Returns the port or -1 on error.
int startEchoServer();

int countOccurrences(std::string_view haystack, std::string_view needle);

bool noBodyAfterHeaders(std::string_view raw);

// Very small blocking GET helper (Connection: close) used by tests that just need
// the full raw HTTP response bytes. Not HTTP-complete (no redirects, TLS, etc.).
std::string simpleGet(uint16_t port, std::string_view path);

// Minimal GET request helper used across compression streaming tests. Parses headers into a map and returns body raw.
ParsedResponse simpleGet(uint16_t port, std::string_view target,
                         std::vector<std::pair<std::string, std::string>> extraHeaders);

std::string toLower(std::string input);

// Very small HTTP/1.1 response parser (not resilient to all malformed cases, just for test consumption)
std::optional<ParsedResponse> parseResponse(const std::string& raw);

ParsedResponse parseResponseOrThrow(const std::string& raw);

bool setRecvTimeout(int fd, ::aeronet::SysDuration timeout);

std::string buildRequest(const RequestOptions& opt);

std::optional<std::string> request(uint16_t port, const RequestOptions& opt = {});

// Convenience wrapper that throws std::runtime_error on failure instead of returning std::nullopt.
// This simplifies test code by eliminating explicit ASSERT checks for has_value(); gtest will treat
// uncaught exceptions as test failures with the diagnostic message.
std::string requestOrThrow(uint16_t port, const RequestOptions& opt = {});

// Send multiple requests over a single keep-alive connection and return raw responses individually.
// Limitations: assumes server responds fully before next request is parsed (sufficient for simple tests).
std::vector<std::string> sequentialRequests(uint16_t port, std::span<const RequestOptions> reqs);

bool AttemptConnect(uint16_t port);

bool WaitForPeerClose(int fd, std::chrono::milliseconds timeout);

bool WaitForListenerClosed(uint16_t port, std::chrono::milliseconds timeout);

}  // namespace aeronet::test
