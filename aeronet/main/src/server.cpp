#include "aeronet/server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <utility>

#include "aeronet/event-loop.hpp"
#include "aeronet/http-error.hpp"
#include "exception.hpp"
#include "flat-hash-map.hpp"
#include "string-equal-ignore-case.hpp"

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

std::string_view findHeader(const HttpHeaders& hdrs, std::string_view key) {
  // Fast path: exact case match first (common)
  if (auto it = hdrs.find(key); it != hdrs.end()) {
    return it->second;
  }
  for (const auto& kv : hdrs) {
    if (CaseInsensitiveEqual(kv.first, key)) {
      return kv.second;
    }
  }
  return {};
}

}  // namespace

HttpServer::HttpServer(const ServerConfig& cfg) : _config(cfg) {}

HttpServer::~HttpServer() { stop(); }

HttpServer::HttpServer(HttpServer&& other) noexcept
    : _listenFd(std::exchange(other._listenFd, -1)),
      _running(std::exchange(other._running, false)),
      _handler(std::move(other._handler)),
      _loop(std::move(other._loop)),
      _config(std::move(other._config)),
      _connStates(std::move(other._connStates)),
      _cachedDate(std::move(other._cachedDate)),
      _cachedDateEpoch(other._cachedDateEpoch) {
  other._config.port = 0;
  other._cachedDate.clear();
  other._cachedDateEpoch = 0;
  other._connStates.clear();
}

HttpServer& HttpServer::operator=(HttpServer&& other) noexcept {
  if (this != &other) {
    stop();
    _listenFd = std::exchange(other._listenFd, -1);
    _running = std::exchange(other._running, false);
    _handler = std::move(other._handler);
    _loop = std::move(other._loop);
    _config = std::move(other._config);
    _connStates = std::move(other._connStates);
    _cachedDate = std::move(other._cachedDate);
    _cachedDateEpoch = other._cachedDateEpoch;
    other._config.port = 0;
    other._cachedDate.clear();
    other._cachedDateEpoch = 0;
    other._connStates.clear();
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
  const int optName = _config.reusePort ? SO_REUSEPORT : SO_REUSEADDR;
  const int enable = 1;
  ::setsockopt(_listenFd, SOL_SOCKET, optName, &enable, sizeof(enable));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(_config.port);
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
  // Per-instance connection state map lives in _connStates
  // Cached date lives in _cachedDate / _cachedDateEpoch

  auto refreshDate = [&]() {
    std::time_t now = std::time(nullptr);
    if (now != _cachedDateEpoch) {
      _cachedDateEpoch = now;
      char buf[64];
      std::tm tmv{};
      gmtime_r(&now, &tmv);
      std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tmv);
      _cachedDate = buf;
    }
  };
  refreshDate();

  auto closeConnection = [&](int cfd) {
    if (_loop) {
      _loop->del(cfd);
    }
    ::close(cfd);
    _connStates.erase(cfd);
  };

  // Idle timeout sweep (cheap linear scan with small connection counts typical for example)
  if (_config.enableKeepAlive) {
    auto now = std::chrono::steady_clock::now();
    for (auto it = _connStates.begin(); it != _connStates.end();) {
      if (it->second.shouldClose || (now - it->second.lastActivity) > _config.keepAliveTimeout) {
        int connFd = it->first;
        ++it;
        closeConnection(connFd);
        continue;
      }
      ++it;
    }
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
        } else {
          _connStates.emplace(client_fd, ConnStateInternal{});
        }
      }
    } else {
      auto itState = _connStates.find(fd);
      if (itState == _connStates.end()) {
        // stale or already closed
        return;
      }
      ConnStateInternal& st = itState->second;
      st.lastActivity = std::chrono::steady_clock::now();
      bool close_conn = false;
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
        st.buffer.append(buf, buf + count);
        if (st.buffer.size() > _config.maxHeaderBytes + _config.maxBodyBytes) {
          close_conn = true;
          break;
        }
        // Process as many complete requests as available (no pipelining write concurrency yet; sequential)
        while (true) {
          // Need full header first
          std::size_t headerEnd = st.buffer.find("\r\n\r\n");
          if (headerEnd == std::string::npos) {
            break;
          }
          if (headerEnd > _config.maxHeaderBytes) {  // headers too large
            sendSimpleError(fd, 431, "Request Header Fields Too Large", _cachedDate, true);
            close_conn = true;
            break;
          }
          // Parse headers portion
          HttpRequest req{};
          const char* begin = st.buffer.data();
          const char* headers_end_ptr = begin + headerEnd;  // points to first '\r'
          // Request line
          const char* raw_line_nl = static_cast<const char*>(std::memchr(begin, '\n', static_cast<size_t>(headerEnd)));
          if (!raw_line_nl) {
            sendSimpleError(fd, 400, "Bad Request", _cachedDate, true);
            close_conn = true;
            break;
          }
          const char* line_start = begin;
          const char* line_end_trim = raw_line_nl;
          if (line_end_trim > line_start && *(line_end_trim - 1) == '\r') {
            --line_end_trim;
          }
          const char* sp1 =
              static_cast<const char*>(std::memchr(line_start, ' ', static_cast<size_t>(line_end_trim - line_start)));
          if (!sp1) {
            sendSimpleError(fd, 400, "Bad Request", _cachedDate, true);
            close_conn = true;
            break;
          }
          const char* sp2 =
              static_cast<const char*>(std::memchr(sp1 + 1, ' ', static_cast<size_t>(line_end_trim - (sp1 + 1))));
          if (!sp2) {
            sendSimpleError(fd, 400, "Bad Request", _cachedDate, true);
            close_conn = true;
            break;
          }
          req.method = {line_start, static_cast<size_t>(sp1 - line_start)};
          req.target = {sp1 + 1, static_cast<size_t>(sp2 - (sp1 + 1))};
          req.version = {sp2 + 1, static_cast<size_t>(line_end_trim - (sp2 + 1))};
          // Supported versions: HTTP/1.0 and HTTP/1.1 only
          if (!(req.version == "HTTP/1.1" || req.version == "HTTP/1.0")) {
            sendSimpleError(fd, 505, "HTTP Version Not Supported", _cachedDate, true);
            close_conn = true;
            break;
          }
          // headers
          const char* cursor = (raw_line_nl < headers_end_ptr) ? (raw_line_nl + 1) : headers_end_ptr;
          while (cursor < headers_end_ptr) {
            const char* next_nl =
                static_cast<const char*>(std::memchr(cursor, '\n', static_cast<size_t>(headers_end_ptr - cursor)));
            if (!next_nl) {
              next_nl = headers_end_ptr;
            }
            const char* line_e = next_nl;
            if (line_e > cursor && *(line_e - 1) == '\r') {
              --line_e;
            }
            if (line_e == cursor) {
              break;
            }
            const char* colon =
                static_cast<const char*>(std::memchr(cursor, ':', static_cast<size_t>(line_e - cursor)));
            if (colon) {
              const char* value_beg = colon + 1;
              while (value_beg < line_e && (*value_beg == ' ' || *value_beg == '\t')) {
                ++value_beg;
              }
              const char* value_end = line_e;
              while (value_end > value_beg && (*(value_end - 1) == ' ' || *(value_end - 1) == '\t')) {
                --value_end;
              }
              req.headers.emplace(
                  std::make_pair(std::string_view(cursor, colon), std::string_view(value_beg, value_end)));
            }
            cursor = (next_nl < headers_end_ptr) ? (next_nl + 1) : headers_end_ptr;
          }
          // Determine body handling (Content-Length or chunked)
          bool isChunked = false;
          size_t contentLen = 0;
          bool hasTE = false;
          if (std::string_view te = findHeader(req.headers, "Transfer-Encoding"); !te.empty()) {
            hasTE = true;
            if (req.version == "HTTP/1.0") {
              // HTTP/1.0 does not support Transfer-Encoding at all
              sendSimpleError(fd, 400, "Bad Request", _cachedDate, true);
              close_conn = true;
              break;
            }
            if (CaseInsensitiveEqual(te, "chunked")) {
              isChunked = true;
            } else {
              sendSimpleError(fd, 501, "Not Implemented", _cachedDate, true);
              close_conn = true;
              break;
            }
          }
          // Detect presence of Content-Length (even if chunked for conflict signaling)
          bool hasCL = false;
          std::string_view lenViewAll = findHeader(req.headers, "Content-Length");
          if (!lenViewAll.empty()) {
            hasCL = true;
          }
          if (hasCL && hasTE) {
            // Per RFC 7230, having both is invalid
            sendSimpleError(fd, 400, "Bad Request", _cachedDate, true);
            close_conn = true;
            break;
          }
          bool sent100 = false;
          auto send100 = [&]() {
            if (sent100) {
              return;
            }
            const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
            ::write(fd, interim, sizeof(interim) - 1);
            sent100 = true;
          };
          bool expectContinue = false;
          if (req.version == "HTTP/1.1") {
            if (std::string_view expectVal = findHeader(req.headers, "Expect"); !expectVal.empty()) {
              if (CaseInsensitiveEqual(expectVal, "100-continue")) {
                expectContinue = true;
              }
            }
          }
          size_t consumedBytes = 0;  // amount to erase from buffer after we finish handling the request
          if (!isChunked) {
            if (hasCL) {
              size_t parsed = 0;
              for (char ch : lenViewAll) {
                if (ch < '0' || ch > '9') {
                  parsed = _config.maxBodyBytes + 1;
                  break;
                }
                parsed = (parsed * 10U) + static_cast<size_t>(ch - '0');
                if (parsed > _config.maxBodyBytes) {
                  break;
                }
              }
              contentLen = parsed;
              if (contentLen > _config.maxBodyBytes) {
                sendSimpleError(fd, 413, "Payload Too Large", _cachedDate, true);
                close_conn = true;
                break;
              }
              if (expectContinue && contentLen > 0) {
                send100();
              }
            }
            size_t totalNeeded = headerEnd + 4 + contentLen;
            if (st.buffer.size() < totalNeeded) {
              break;  // wait for more body bytes
            }
            const char* body_start = st.buffer.data() + headerEnd + 4;
            req.body = {body_start, contentLen};
            consumedBytes = totalNeeded;
          } else {
            // Chunked decoding: we need full set of chunks ending with 0\r\n\r\n
            size_t headerAndSep = headerEnd + 4;  // starting offset of first chunk size line
            size_t pos = headerAndSep;
            std::string decodedBody;
            decodedBody.reserve(1024);
            bool needMore = false;
            if (expectContinue) {
              send100();
            }
            while (true) {
              // find end of size line
              size_t lineEnd = st.buffer.find("\r\n", pos);
              if (lineEnd == std::string::npos) {
                needMore = true;
                break;
              }
              std::string_view sizeLine(st.buffer.data() + pos, lineEnd - pos);
              // parse hex size
              size_t chunkSize = 0;
              for (char ch : sizeLine) {
                if (ch == ';') {
                  break;
                }  // ignore extensions
                unsigned value = 0;
                if (ch >= '0' && ch <= '9') {
                  value = static_cast<unsigned>(ch - '0');
                } else if (ch >= 'a' && ch <= 'f') {
                  value = static_cast<unsigned>(10 + (ch - 'a'));
                } else if (ch >= 'A' && ch <= 'F') {
                  value = static_cast<unsigned>(10 + (ch - 'A'));
                } else {
                  chunkSize = _config.maxBodyBytes + 1;
                  break;
                }
                chunkSize = (chunkSize << 4) | static_cast<size_t>(value);
                if (chunkSize > _config.maxBodyBytes) {
                  break;
                }
              }
              pos = lineEnd + 2;  // skip CRLF after size line
              if (chunkSize > _config.maxBodyBytes) {
                sendSimpleError(fd, 413, "Payload Too Large", _cachedDate, true);
                close_conn = true;
                break;
              }
              if (st.buffer.size() < pos + chunkSize + 2) {
                needMore = true;
                break;
              }
              if (chunkSize == 0) {
                // Final chunk. We ignore trailers for now; need just the terminating CRLF.
                if (st.buffer.size() < pos + 2) {
                  needMore = true;
                  break;
                }
                if (st.buffer.compare(pos, 2, "\r\n") != 0) {
                  needMore = true;  // malformed or incomplete
                  break;
                }
                pos += 2;  // consume final CRLF
                break;
              }
              decodedBody.append(st.buffer, pos, chunkSize);
              if (decodedBody.size() > _config.maxBodyBytes) {
                sendSimpleError(fd, 413, "Payload Too Large", _cachedDate, true);
                close_conn = true;
                break;
              }
              pos += chunkSize;
              if (st.buffer.compare(pos, 2, "\r\n") != 0) {
                needMore = true;
                break;
              }
              pos += 2;  // skip CRLF after data
            }
            if (close_conn) {
              break;
            }
            if (needMore) {
              // wait for more bytes
              break;
            }
            // Move decoded body into connection's persistent storage to keep lifetime
            st.bodyStorage.assign(decodedBody.data(), decodedBody.size());
            req.body = std::string_view(st.bodyStorage);
            consumedBytes = pos;
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
          string body = resp.body;
          bool isHead = (req.method.size() == 4 && (req.method == "HEAD"));
          // HTTP/1.1: persistent by default (unless Connection: close). HTTP/1.0: non-persistent unless keep-alive
          // token present.
          bool keepAlive = false;
          if (_config.enableKeepAlive) {
            if (req.version == "HTTP/1.1") {
              keepAlive = true;  // default on
            } else if (req.version == "HTTP/1.0") {
              keepAlive = false;  // default off
            }
            if (std::string_view connVal = findHeader(req.headers, "Connection"); !connVal.empty()) {
              if (CaseInsensitiveEqual(connVal, "close")) {
                keepAlive = false;
              } else if (CaseInsensitiveEqual(connVal, "keep-alive")) {
                keepAlive = true;  // opt-in (works for 1.0)
              }
            }
          }
          ++st.requestsServed;
          if (st.requestsServed >= _config.maxRequestsPerConnection) {
            keepAlive = false;
          }
          auto header = resp.buildHead(req.version, _cachedDate, keepAlive, body.size());
          if (isHead) {
            // For HEAD we must send only headers; body length reflects what a GET would have sent.
            ::write(fd, header.data(), header.size());
          } else {
            // Avoid extra allocation by using scatter-gather write (writev) for header + body.
            // NOTE: Partial writes are not handled here (same as previous single write). For very
            // large responses or backpressure scenarios, production code should retain unsent
            // segments and re-arm EPOLLOUT.
            struct iovec iov[2];
            iov[0].iov_base = const_cast<char*>(header.data());
            iov[0].iov_len = header.size();
            iov[1].iov_base = const_cast<char*>(body.data());
            iov[1].iov_len = body.size();
            ::writev(fd, iov, 2);
          }
          // Erase consumed bytes from buffer AFTER handler & response prep so that string_view fields remain valid.
          if (consumedBytes > 0) {
            st.buffer.erase(0, consumedBytes);
          }
          if (!keepAlive) {
            close_conn = true;
          }
          if (close_conn) {
            break;
          }  // stop processing further pipelined requests
        }
        if (close_conn) {
          break;
        }  // exit read loop
      }
      if (close_conn) {
        closeConnection(fd);
      }
    }
  });
}

}  // namespace aeronet
