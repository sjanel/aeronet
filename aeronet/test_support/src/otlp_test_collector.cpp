#include "aeronet/otlp_test_collector.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/base-fd.hpp"
#include "aeronet/errno_throw.hpp"
#include "aeronet/log.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/test_util.hpp"

namespace aeronet::test {
namespace {
std::string_view trimLeading(std::string_view sv) {
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
    sv.remove_prefix(1);
  }
  return sv;
}

std::size_t parseContentLength(std::string_view headers) {
  std::size_t cursor = 0;
  while (cursor < headers.size()) {
    auto next = headers.find("\r\n", cursor);
    if (next == std::string::npos) {
      next = headers.size();
    }
    std::string_view line(headers.data() + cursor, next - cursor);
    cursor = next == headers.size() ? next : next + 2;
    if (line.empty()) {
      continue;
    }
    auto colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    std::string_view name = line.substr(0, colon);
    if (!CaseInsensitiveEqual(name, "Content-Length")) {
      continue;
    }
    std::string_view value = trimLeading(line.substr(colon + 1));
    std::size_t len = 0;
    const auto* first = value.data();
    const auto* last = first + value.size();
    auto [ptr, ec] = std::from_chars(first, last, len);
    if (ec != std::errc() || ptr != last) {
      throw std::runtime_error("invalid Content-Length header");
    }
    return len;
  }
  throw std::runtime_error("Content-Length header missing");
}

CapturedOtlpRequest parseHeadAndBody(std::string_view request, std::string_view body) {
  CapturedOtlpRequest out;
  auto lineEnd = request.find("\r\n");
  if (lineEnd == std::string::npos) {
    throw std::runtime_error("malformed HTTP request line");
  }
  std::string_view requestLine = request.substr(0, lineEnd);
  auto firstSpace = requestLine.find(' ');
  auto secondSpace =
      firstSpace == std::string_view::npos ? std::string_view::npos : requestLine.find(' ', firstSpace + 1);
  if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos) {
    throw std::runtime_error("malformed HTTP request line fields");
  }
  out.method.assign(requestLine.substr(0, firstSpace));
  out.path.assign(requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1));

  std::size_t cursor = lineEnd + 2;  // skip CRLF
  while (cursor < request.size()) {
    auto next = request.find("\r\n", cursor);
    if (next == std::string::npos) {
      break;
    }
    if (next == cursor) {
      cursor += 2;
      break;  // reached end of headers
    }
    std::string_view line(request.data() + cursor, next - cursor);
    cursor = next + 2;
    auto colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    std::string_view name = line.substr(0, colon);
    std::string_view value = trimLeading(line.substr(colon + 1));
    out.headers.emplace_back(std::string(name), std::string(value));
  }

  out.body.assign(body.begin(), body.end());
  return out;
}
}  // namespace

OtlpTestCollector::OtlpTestCollector() : _listen(SOCK_STREAM) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // ephemeral
  if (::bind(_listen.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    throw_errno("bind OTLP collector socket");
  }
  if (::listen(_listen.fd(), 8) != 0) {
    throw_errno("listen OTLP collector socket");
  }
  sockaddr_in actual{};
  socklen_t len = sizeof(actual);
  if (::getsockname(_listen.fd(), reinterpret_cast<sockaddr*>(&actual), &len) != 0) {
    throw_errno("getsockname for OTLP collector");
  }
  _port = ntohs(actual.sin_port);

  int flags = ::fcntl(_listen.fd(), F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(_listen.fd(), F_SETFL, flags | O_NONBLOCK);
  }

  _thread = std::jthread([this] { run(); });
}

OtlpTestCollector::~OtlpTestCollector() {
  _stop.store(true);
  if (_listen) {
    ::shutdown(_listen.fd(), SHUT_RDWR);
    _listen.close();
  }
}

void OtlpTestCollector::run() {
  while (!_stop.load()) {
    if (!acceptOnce()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
  }
}

bool OtlpTestCollector::acceptOnce() {
  if (!_listen) {
    return false;
  }
  pollfd pfd{};
  pfd.fd = _listen.fd();
  pfd.events = POLLIN;
  constexpr int kPollMs = 25;
  const int ready = ::poll(&pfd, 1, kPollMs);
  if (ready <= 0) {
    return false;
  }
  BaseFd client(::accept(_listen.fd(), nullptr, nullptr));
  if (!client) {
    return true;
  }
  handleClient(client.release());
  return true;
}

void OtlpTestCollector::handleClient(int clientFd) {
  BaseFd client(clientFd);
  std::string buffer;
  buffer.reserve(4096);
  while (!_stop.load()) {
    char chunk[4096];
    const ssize_t received = ::recv(client.fd(), chunk, sizeof(chunk), 0);
    if (received == 0) {
      break;
    }
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        continue;
      }
      log::error("OTLP test collector recv failed: {}", std::strerror(errno));
      return;
    }
    buffer.append(chunk, static_cast<std::size_t>(received));
    auto headEnd = buffer.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
      continue;
    }
    std::string_view headers(buffer.data(), headEnd);
    std::size_t bodyLen = 0;
    try {
      bodyLen = parseContentLength(headers);
    } catch (const std::exception& ex) {
      log::error("OTLP test collector failed to parse Content-Length: {}", ex.what());
      return;
    }
    const std::size_t totalNeeded = headEnd + 4 + bodyLen;
    if (buffer.size() < totalNeeded) {
      continue;
    }
    std::string_view rawHead(buffer.data(), headEnd + 4);
    std::string_view body(buffer.data() + headEnd + 4, bodyLen);
    try {
      auto req = parseHeadAndBody(rawHead, body);
      recordRequest(std::move(req));
    } catch (const std::exception& ex) {
      log::error("OTLP test collector failed to parse request: {}", ex.what());
    }
    try {
      static constexpr std::string_view kOkResponse =
          "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
      sendAll(client.fd(), kOkResponse, std::chrono::milliseconds{1000});
    } catch (const std::exception& ex) {
      log::error("OTLP test collector failed to reply: {}", ex.what());
    }
    break;
  }
}

void OtlpTestCollector::recordRequest(CapturedOtlpRequest req) {
  {
    std::scoped_lock lk(_mutex);
    _requests.emplace_back(std::move(req));
  }
  _cv.notify_one();
}

CapturedOtlpRequest OtlpTestCollector::waitForRequest(std::chrono::milliseconds timeout) {
  std::unique_lock lk(_mutex);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  if (!_cv.wait_until(lk, deadline, [&] { return !_requests.empty() || _stop.load(); })) {
    throw std::runtime_error("timed out waiting for OTLP request");
  }
  if (_requests.empty()) {
    throw std::runtime_error("collector stopped before receiving request");
  }
  CapturedOtlpRequest req = std::move(_requests.front());
  _requests.pop_front();
  return req;
}

std::vector<CapturedOtlpRequest> OtlpTestCollector::drain() {
  std::vector<CapturedOtlpRequest> out;
  std::scoped_lock lk(_mutex);
  while (!_requests.empty()) {
    out.emplace_back(std::move(_requests.front()));
    _requests.pop_front();
  }
  return out;
}

std::string OtlpTestCollector::endpointForTraces() const { return endpointForPath("/v1/traces"); }

std::string OtlpTestCollector::endpointForPath(std::string_view path) const {
  std::string normalized(path);
  if (normalized.empty() || normalized.front() != '/') {
    normalized.insert(normalized.begin(), '/');
  }
  std::string url = "http://127.0.0.1:" + std::to_string(_port);
  url.append(normalized);
  return url;
}

std::string CapturedOtlpRequest::headerValue(std::string_view name) const {
  for (const auto& entry : headers) {
    if (CaseInsensitiveEqual(entry.first, name)) {
      return entry.second;
    }
  }
  return {};
}

}  // namespace aeronet::test
