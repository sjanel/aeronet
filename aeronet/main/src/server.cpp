#define AMC_NONSTD_FEATURES

#include "aeronet/server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <utility>

#include "aeronet/event_loop.hpp"
#include "exception.hpp"

namespace aeronet {
namespace {
int set_non_blocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    return -1;
  }
  return 0;
}
}  // namespace

HttpServer::HttpServer(uint16_t port) : _port(port) {}

HttpServer::~HttpServer() { stop(); }

HttpServer::HttpServer(HttpServer&& other) noexcept
    : _listenFd(std::exchange(other._listenFd, -1)),
      _port(std::exchange(other._port, 0)),
      _running(std::exchange(other._running, false)),
      _reusePort(std::exchange(other._reusePort, false)),
      _handler(std::move(other._handler)),
      _loop(std::move(other._loop)) {}

HttpServer& HttpServer::operator=(HttpServer&& other) noexcept {
  if (this != &other) {
    stop();  // ensures current fd + loop closed
    _listenFd = std::exchange(other._listenFd, -1);
    _port = std::exchange(other._port, 0);
    _running = std::exchange(other._running, false);
    _reusePort = std::exchange(other._reusePort, false);
    _handler = std::move(other._handler);
    _loop = std::move(other._loop);
  }
  return *this;
}

void HttpServer::setHandler(RequestHandler handler) { _handler = std::move(handler); }

void HttpServer::setupListener() {
  if (_listenFd != -1) {
    throw exception("Server is already listening");
  }
  _listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (_listenFd < 0) {
    throw std::runtime_error("socket failed");
  }
  const int optName = _reusePort ? SO_REUSEPORT : SO_REUSEADDR;
  const int enable = 1;
  ::setsockopt(_listenFd, SOL_SOCKET, optName, &enable, sizeof(enable));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(_port);
  if (bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    throw std::runtime_error("bind failed");
  }
  if (listen(_listenFd, SOMAXCONN) < 0) {
    throw std::runtime_error("listen failed");
  }
  if (set_non_blocking(_listenFd) < 0) {
    throw std::runtime_error("failed to set non-blocking");
  }
  if (_loop == nullptr) {
    _loop = std::make_unique<EventLoop>();
  }
  if (!_loop->add(_listenFd, EPOLLIN)) {
    throw std::runtime_error("EventLoop add listen socket failed");
  }
}

void HttpServer::run() {
  if (_running) {
    throw exception("Server is already running");
  }
  _running = true;
  setupListener();
  while (_running) {
    eventLoop(500);
  }
}

void HttpServer::stop() {
  _running = false;
  if (_listenFd != -1) {
    ::close(_listenFd);
    _listenFd = -1;
  }
  _loop.reset();
}

void HttpServer::runUntil(const std::function<bool()>& predicate, std::chrono::milliseconds checkPeriod) {
  if (_running) {
    return;
  }
  _running = true;
  setupListener();
  int timeoutMs = static_cast<int>(checkPeriod.count());
  timeoutMs = std::max(timeoutMs, 1);
  while (_running) {
    eventLoop(timeoutMs);
    if (predicate && predicate()) {
      stop();
    }
  }
}

void HttpServer::eventLoop(int timeoutMs) {
  if (!_loop) {
    return;
  }
  _loop->poll(timeoutMs, [&](int fd, [[maybe_unused]] uint32_t ev) {
    if (fd == _listenFd) {
      while (true) {
        sockaddr_in in_addr{};
        socklen_t in_len = sizeof(in_addr);
        int client_fd = accept(_listenFd, reinterpret_cast<sockaddr*>(&in_addr), &in_len);
        if (client_fd < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          }
          std::perror("accept");
          break;
        }
        set_non_blocking(client_fd);
        if (!_loop->add(client_fd, EPOLLIN | EPOLLET)) {
          std::perror("EventLoop add client");
          ::close(client_fd);
        }
      }
    } else {
      bool close_conn = false;
      std::string request_buf;  // backing storage for request (headers + optional body)
      char buf[4096];
      while (true) {
        ssize_t count = ::read(fd, buf, sizeof(buf));
        if (count < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          }
          std::perror("read");
          close_conn = true;
          break;
        }
        if (count == 0) {
          close_conn = true;
          break;
        }
        request_buf.append(buf, buf + count);
        if (request_buf.size() > 8192) {
          close_conn = true;
          break;
        }
        if (request_buf.find("\r\n\r\n") != std::string::npos) {
          break;
        }
      }
      if (!request_buf.empty()) {
        HttpRequest req{};
        const char* begin = request_buf.data();
        const char* end = begin + request_buf.size();
        // Find end of headers marker
        const std::string_view all(request_buf.data(), request_buf.size());
        std::size_t header_end_pos = all.find("\r\n\r\n");
        if (header_end_pos == std::string::npos) {
          // incomplete headers (should not happen here) close
          close_conn = true;
        } else {
          const char* headers_end_ptr = begin + header_end_pos;  // points to first '\r' of delimiter
          // Parse request line
          // Locate the newline terminating the request line
          const char* raw_line_nl =
              static_cast<const char*>(std::memchr(begin, '\n', static_cast<size_t>(headers_end_ptr - begin)));
          if (!raw_line_nl) {
            raw_line_nl = headers_end_ptr;  // fallback (malformed but continue defensively)
          }
          const char* line_start = begin;
          const char* line_end_trim = raw_line_nl;  // will be trimmed of optional CR for slicing the request line
          if (line_end_trim > line_start && *(line_end_trim - 1) == '\r') {
            --line_end_trim;  // exclude trailing CR from the request-line view
          }
          // request line: METHOD SP TARGET SP VERSION
          const char* sp1 =
              static_cast<const char*>(std::memchr(line_start, ' ', static_cast<size_t>(line_end_trim - line_start)));
          if (sp1) {
            const char* sp2 =
                static_cast<const char*>(std::memchr(sp1 + 1, ' ', static_cast<size_t>(line_end_trim - (sp1 + 1))));
            if (sp2) {
              req.method = std::string_view(line_start, static_cast<std::size_t>(sp1 - line_start));
              req.target = std::string_view(sp1 + 1, static_cast<std::size_t>(sp2 - (sp1 + 1)));
              req.version = std::string_view(sp2 + 1, static_cast<std::size_t>(line_end_trim - (sp2 + 1)));
            }
          }
          // Parse header lines. Advance starting point to the byte AFTER the newline of the request line.
          const char* cursor = (raw_line_nl < headers_end_ptr) ? (raw_line_nl + 1) : headers_end_ptr;
          while (cursor < headers_end_ptr) {
            const char* next_nl =
                static_cast<const char*>(std::memchr(cursor, '\n', static_cast<size_t>(headers_end_ptr - cursor)));
            if (!next_nl) {
              next_nl = headers_end_ptr;  // last line
            }
            const char* line_e = next_nl;  // exclusive
            if (line_e > cursor && *(line_e - 1) == '\r') {
              --line_e;
            }
            if (line_e == cursor) {  // empty line (shouldn't before header_end) break
              break;
            }
            // key: value
            const char* colon =
                static_cast<const char*>(std::memchr(cursor, ':', static_cast<size_t>(line_e - cursor)));
            if (colon) {
              const char* value_beg = colon + 1;
              // Trim leading SP / HTAB
              while (value_beg < line_e && (*value_beg == ' ' || *value_beg == '\t')) {
                ++value_beg;
              }
              // Trim trailing spaces
              const char* value_end = line_e;
              while (value_end > value_beg && (*(value_end - 1) == ' ' || *(value_end - 1) == '\t')) {
                --value_end;
              }
              req.headers.emplace(
                  std::make_pair(std::string_view(cursor, colon), std::string_view(value_beg, value_end)));
            }
            cursor = (next_nl < headers_end_ptr) ? (next_nl + 1) : headers_end_ptr;
          }
          // Body view (if any)
          const char* body_start = headers_end_ptr + 4;  // skip \r\n\r\n
          if (body_start <= end) {
            req.body = std::string_view(body_start, static_cast<std::string_view::size_type>(end - body_start));
          }
        }
        HttpResponse resp;
        if (_handler) {
          try {
            resp = _handler(req);
          } catch (const std::exception& ex) {
            std::cerr << "Exception in request handler: " << ex.what() << '\n';
            resp.statusCode = 500;
            resp.reason = "Internal Server Error";
            resp.body = "Internal Server Error";
            resp.contentType = "text/plain";
          } catch (...) {
            std::cerr << "Unknown exception in request handler." << '\n';
            resp.statusCode = 500;
            resp.reason = "Internal Server Error";
            resp.body = "Internal Server Error";
            resp.contentType = "text/plain";
          }
        }
        std::string body = resp.body;
        std::string header = "HTTP/1.1 " + std::to_string(resp.statusCode) + " " + resp.reason + "\r\n";
        header += "Content-Type: " + resp.contentType + "\r\n";
        header += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        header += "Connection: close\r\n\r\n";
        std::string out = header + body;
        ssize_t bytesWritten = ::write(fd, out.data(), out.size());
        (void)bytesWritten;
        close_conn = true;
      }
      if (close_conn) {
        _loop->del(fd);
        ::close(fd);
      }
    }
  });
}

}  // namespace aeronet
