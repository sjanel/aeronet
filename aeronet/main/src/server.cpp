#include "aeronet/server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>  // for struct iovec
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <type_traits>
#include <utility>

#include "aeronet/event-loop.hpp"
#include "exception.hpp"
#include "flat-hash-map.hpp"
#include "http-constants.hpp"
#include "http-error-build.hpp"
#include "http-method-build.hpp"
#include "http-method.hpp"
#include "string-equal-ignore-case.hpp"
#include "timestring.hpp"

namespace aeronet {
namespace {
int setNonBlocking(int fd) {
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

HttpServer::HttpServer(const ServerConfig& cfg) : _config(cfg) {}

HttpServer::~HttpServer() { stop(); }

HttpServer::HttpServer(HttpServer&& other) noexcept
    : _listenFd(std::exchange(other._listenFd, -1)),
      _running(std::exchange(other._running, false)),
      _handler(std::move(other._handler)),
      _pathHandlers(std::move(other._pathHandlers)),
      _loop(std::move(other._loop)),
      _config(std::move(other._config)),
      _connStates(std::move(other._connStates)),
      _cachedDate(std::move(other._cachedDate)),
      _cachedDateEpoch(std::exchange(other._cachedDateEpoch, TimePoint{})),
      _parserErrCb(std::move(other._parserErrCb)) {}

HttpServer& HttpServer::operator=(HttpServer&& other) noexcept {
  if (this != &other) {
    stop();
    _listenFd = std::exchange(other._listenFd, -1);
    _running = std::exchange(other._running, false);
    _handler = std::move(other._handler);
    _pathHandlers = std::move(other._pathHandlers);
    _loop = std::move(other._loop);
    _config = std::move(other._config);
    _connStates = std::move(other._connStates);
    _cachedDate = std::move(other._cachedDate);
    _cachedDateEpoch = std::exchange(other._cachedDateEpoch, TimePoint{});
    _parserErrCb = std::move(other._parserErrCb);
  }
  return *this;
}

void HttpServer::setHandler(RequestHandler handler) { _handler = std::move(handler); }

void HttpServer::addPathHandler(std::string_view path, const http::MethodSet& methods, const RequestHandler& handler) {
  if (_handler) {
    throw exception("Cannot use addPathHandler after setHandler has been set");
  }
  auto it = _pathHandlers.find(path);
  PathHandlerEntry* pPathHandlerEntry;
  if (it == _pathHandlers.end()) {
    pPathHandlerEntry = &_pathHandlers[string(path)];
  } else {
    pPathHandlerEntry = &it->second;
  }
  pPathHandlerEntry->methodMask = http::methodListToMask(methods);
  for (http::Method method : methods) {
    pPathHandlerEntry->handlers[static_cast<std::underlying_type_t<http::Method>>(method)] = handler;
  }
}

void HttpServer::addPathHandler(std::string_view path, http::Method method, const RequestHandler& handler) {
  if (_handler) {
    throw exception("Cannot use addPathHandler after setHandler has been set");
  }
  auto it = _pathHandlers.find(path);
  PathHandlerEntry* pPathHandlerEntry;
  if (it == _pathHandlers.end()) {
    pPathHandlerEntry = &_pathHandlers[string(path)];
  } else {
    pPathHandlerEntry = &it->second;
  }
  pPathHandlerEntry->methodMask = http::singleMethodToMask(method);
  pPathHandlerEntry->handlers[static_cast<std::underlying_type_t<http::Method>>(method)] = handler;
}

void HttpServer::setupListener() {
  if (_listenFd != -1) {
    throw exception("Server is already listening");
  }
  _listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (_listenFd < 0) {
    throw std::runtime_error("socket failed");
  }
  // Always enable SO_REUSEADDR for quick restarts; enable SO_REUSEPORT when requested
  static constexpr int enable = 1;
  ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  if (_config.reusePort) {
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));
  }
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
  // If user requested ephemeral port (0) capture actual assigned port.
  if (_config.port == 0) {
    sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (::getsockname(_listenFd, reinterpret_cast<sockaddr*>(&actual), &alen) == 0) {
      _config.port = ntohs(actual.sin_port);
    }
  }
  if (setNonBlocking(_listenFd) < 0) {
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
    eventLoop(std::chrono::milliseconds{500});
  }
}

void HttpServer::stop() {
  _running = false;
  if (_listenFd != -1) {
    ::close(_listenFd);
    _listenFd = -1;
  }
}

void HttpServer::runUntil(const std::function<bool()>& predicate, Duration checkPeriod) {
  if (_running) {
    return;
  }
  _running = true;
  setupListener();
  while (_running) {
    eventLoop(checkPeriod);
    if (predicate && predicate()) {
      stop();
    }
  }
}

void HttpServer::refreshCachedDate() {
  using namespace std::chrono;
  TimePoint nowTp = Clock::now();
  auto nowSec = time_point_cast<seconds>(nowTp);
  if (time_point_cast<seconds>(_cachedDateEpoch) != nowSec) {
    _cachedDateEpoch = nowSec;
    char buf[29];
    char* end = TimeToStringRFC7231(nowSec, buf);
    assert(end <= buf + sizeof(buf));
    _cachedDate.assign(buf, static_cast<size_t>(end - buf));
  }
}

void HttpServer::sweepIdleConnections() {
  if (!_config.enableKeepAlive) {
    return;
  }
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

void HttpServer::acceptNewConnections() {
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
    setNonBlocking(client_fd);
    if (!_loop->add(client_fd, EPOLLIN | EPOLLET)) {
      std::perror("EventLoop add client");
      ::close(client_fd);
    } else {
      _connStates.emplace(client_fd, ConnStateInternal{});
    }
  }
}

void HttpServer::closeConnection(int cfd) {
  if (_loop) {
    _loop->del(cfd);
  }
  ::close(cfd);
  _connStates.erase(cfd);
}

bool HttpServer::processRequestsOnConnection(int fd, HttpServer::ConnStateInternal& state) {
  bool closeCnx = false;
  while (true) {
    std::size_t headerEnd = 0;
    HttpRequest req{};
    if (!parseNextRequestFromBuffer(fd, state, req, headerEnd, closeCnx)) {
      break;  // need more data or connection closed
    }
    bool isChunked = false;
    size_t contentLen = 0;
    bool hasTE = false;
    if (std::string_view te = req.findHeader(http::TransferEncoding); !te.empty()) {
      hasTE = true;
      if (req.version == http::HTTP10) {
        string err;
        buildSimpleError(err, 400, http::ReasonBadRequest, _cachedDate, true);
        queueData(fd, state, err.data(), err.size());
        closeCnx = true;
        break;
      }
      if (CaseInsensitiveEqual(te, http::chunked)) {
        isChunked = true;
      } else {
        string err;
        buildSimpleError(err, 501, http::ReasonNotImplemented, _cachedDate, true);
        queueData(fd, state, err.data(), err.size());
        closeCnx = true;
        break;
      }
    }
    bool hasCL = false;
    std::string_view lenViewAll = req.findHeader(http::ContentLength);
    if (!lenViewAll.empty()) {
      hasCL = true;
    }
    if (hasCL && hasTE) {
      string err;
      buildSimpleError(err, 400, http::ReasonBadRequest, _cachedDate, true);
      queueData(fd, state, err.data(), err.size());
      closeCnx = true;
      break;
    }
    bool sent100 = false;
    auto send100 = [&]() {
      if (sent100) {
        return;
      }
      queueData(fd, state, http::HTTP11_100_CONTINUE.data(), http::HTTP11_100_CONTINUE.size());
      sent100 = true;
    };
    bool expectContinue = false;
    if (req.version == http::HTTP11) {
      if (std::string_view expectVal = req.findHeader(http::Expect); !expectVal.empty()) {
        if (CaseInsensitiveEqual(expectVal, http::h100_continue)) {
          expectContinue = true;
        }
      }
    }
    size_t consumedBytes = 0;
    if (!decodeBodyIfReady(fd, state, req, headerEnd, isChunked, expectContinue, closeCnx, consumedBytes)) {
      break;  // need more bytes or error
    }
    HttpResponse resp;
    if (!_pathHandlers.empty()) {
      auto it = _pathHandlers.find(std::string_view(req.target));
      if (it == _pathHandlers.end()) {
        resp.statusCode = 404;
        resp.reason = "Not Found";
        resp.body = "Not Found";
        resp.contentType = "text/plain";
      } else {
        auto method = http::toMethodEnum(req.method);
        if (!http::methodAllowed(it->second.methodMask, method)) {
          resp.statusCode = 405;
          resp.reason = string(http::ReasonMethodNotAllowed);
          resp.body = string(http::ReasonMethodNotAllowed);
          resp.contentType = "text/plain";
        } else {
          try {
            resp = it->second.handlers[static_cast<std::underlying_type_t<http::Method>>(method)](req);
          } catch (const std::exception& ex) {
            std::cerr << "Exception in path handler: " << ex.what() << '\n';
            resp.statusCode = 500;
            resp.reason = string(http::ReasonInternalServerError);
            resp.body = string(http::ReasonInternalServerError);
            resp.contentType = "text/plain";
          } catch (...) {
            std::cerr << "Unknown exception in path handler." << '\n';
            resp.statusCode = 500;
            resp.reason = string(http::ReasonInternalServerError);
            resp.body = string(http::ReasonInternalServerError);
            resp.contentType = "text/plain";
          }
        }
      }
    } else if (_handler) {
      try {
        resp = _handler(req);
      } catch (const std::exception& ex) {
        std::cerr << "Exception in request handler: " << ex.what() << '\n';
        resp.statusCode = 500;
        resp.reason = string(http::ReasonInternalServerError);
        resp.body = string(http::ReasonInternalServerError);
        resp.contentType = "text/plain";
      } catch (...) {
        std::cerr << "Unknown exception in request handler." << '\n';
        resp.statusCode = 500;
        resp.reason = string(http::ReasonInternalServerError);
        resp.body = string(http::ReasonInternalServerError);
        resp.contentType = "text/plain";
      }
    }
    finalizeAndSendResponse(fd, state, req, resp, consumedBytes, closeCnx);
    if (closeCnx) {
      break;
    }
  }
  return closeCnx;
}

bool HttpServer::parseNextRequestFromBuffer(int fd, ConnStateInternal& state, HttpRequest& outReq,
                                            std::size_t& headerEnd, bool& closeConn) {
  static constexpr std::string_view kDoubleCRLF = "\r\n\r\n";
  auto rng = std::ranges::search(state.buffer, kDoubleCRLF);
  if (rng.empty()) {
    return false;  // need more bytes
  }
  headerEnd = rng.begin() - state.buffer.data();
  if (headerEnd > _config.maxHeaderBytes) {
    string err;
    buildSimpleError(err, 431, http::ReasonHeadersTooLarge, _cachedDate, true);
    queueData(fd, state, err.data(), err.size());
    if (_parserErrCb) {
      _parserErrCb(ParserError::HeadersTooLarge);
    }
    closeConn = true;
    return false;
  }
  const char* begin = state.buffer.data();
  const char* headers_end_ptr = begin + headerEnd;
  const char* raw_line_nl = static_cast<const char*>(std::memchr(begin, '\n', static_cast<size_t>(headerEnd)));
  if (raw_line_nl == nullptr) {
    string err;
    buildSimpleError(err, 400, http::ReasonBadRequest, _cachedDate, true);
    queueData(fd, state, err.data(), err.size());
    if (_parserErrCb) {
      _parserErrCb(ParserError::BadRequestLine);
    }
    closeConn = true;
    return false;
  }
  const char* line_start = begin;
  const char* line_end_trim = raw_line_nl;
  if (line_end_trim > line_start && *(line_end_trim - 1) == '\r') {
    --line_end_trim;
  }
  const char* sp1 =
      static_cast<const char*>(std::memchr(line_start, ' ', static_cast<size_t>(line_end_trim - line_start)));
  if (sp1 == nullptr) {
    string err;
    buildSimpleError(err, 400, http::ReasonBadRequest, _cachedDate, true);
    queueData(fd, state, err.data(), err.size());
    if (_parserErrCb) {
      _parserErrCb(ParserError::BadRequestLine);
    }
    closeConn = true;
    return false;
  }
  const char* sp2 = static_cast<const char*>(std::memchr(sp1 + 1, ' ', static_cast<size_t>(line_end_trim - (sp1 + 1))));
  if (sp2 == nullptr) {
    string err;
    buildSimpleError(err, 400, http::ReasonBadRequest, _cachedDate, true);
    queueData(fd, state, err.data(), err.size());
    if (_parserErrCb) {
      _parserErrCb(ParserError::BadRequestLine);
    }
    closeConn = true;
    return false;
  }
  outReq.method = {line_start, static_cast<size_t>(sp1 - line_start)};
  outReq.target = {sp1 + 1, static_cast<size_t>(sp2 - (sp1 + 1))};
  outReq.version = {sp2 + 1, static_cast<size_t>(line_end_trim - (sp2 + 1))};
  if (!(outReq.version == http::HTTP11 || outReq.version == http::HTTP10)) {
    string err;
    buildSimpleError(err, 505, http::ReasonHTTPVersionNotSupported, _cachedDate, true);
    queueData(fd, state, err.data(), err.size());
    if (_parserErrCb) {
      _parserErrCb(ParserError::VersionUnsupported);
    }
    closeConn = true;
    return false;
  }
  const char* cursor = (raw_line_nl < headers_end_ptr) ? (raw_line_nl + 1) : headers_end_ptr;
  while (cursor < headers_end_ptr) {
    const char* next_nl =
        static_cast<const char*>(std::memchr(cursor, '\n', static_cast<size_t>(headers_end_ptr - cursor)));
    if (next_nl == nullptr) {
      next_nl = headers_end_ptr;
    }
    const char* line_e = next_nl;
    if (line_e > cursor && *(line_e - 1) == '\r') {
      --line_e;
    }
    if (line_e == cursor) {
      break;
    }
    const char* colon = static_cast<const char*>(std::memchr(cursor, ':', static_cast<size_t>(line_e - cursor)));
    if (colon != nullptr) {
      const char* value_beg = colon + 1;
      while (value_beg < line_e && (*value_beg == ' ' || *value_beg == '\t')) {
        ++value_beg;
      }
      const char* value_end = line_e;
      while (value_end > value_beg && (*(value_end - 1) == ' ' || *(value_end - 1) == '\t')) {
        --value_end;
      }
      outReq.headers.emplace(std::make_pair(std::string_view(cursor, colon), std::string_view(value_beg, value_end)));
    }
    cursor = (next_nl < headers_end_ptr) ? (next_nl + 1) : headers_end_ptr;
  }
  return true;
}

bool HttpServer::decodeBodyIfReady(int fd, ConnStateInternal& state, const HttpRequest& req, std::size_t headerEnd,
                                   bool isChunked, bool expectContinue, bool& closeConn, size_t& consumedBytes) {
  consumedBytes = 0;
  if (!isChunked) {
    return decodeFixedLengthBody(fd, state, req, headerEnd, expectContinue, closeConn, consumedBytes);
  }
  return decodeChunkedBody(fd, state, req, headerEnd, expectContinue, closeConn, consumedBytes);
}

bool HttpServer::decodeFixedLengthBody(int fd, ConnStateInternal& state, const HttpRequest& req, std::size_t headerEnd,
                                       bool expectContinue, bool& closeConn, size_t& consumedBytes) {
  std::string_view lenViewAll = req.findHeader(http::ContentLength);
  bool hasCL = !lenViewAll.empty();
  size_t contentLen = 0;
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
      string err;
      buildSimpleError(err, 413, http::ReasonPayloadTooLarge, _cachedDate, true);
      queueData(fd, state, err.data(), err.size());
      closeConn = true;
      return false;
    }
    if (expectContinue && contentLen > 0) {
      queueData(fd, state, http::HTTP11_100_CONTINUE.data(), http::HTTP11_100_CONTINUE.size());
    }
  }
  size_t totalNeeded = headerEnd + 4 + contentLen;
  if (state.buffer.size() < totalNeeded) {
    return false;  // need more bytes
  }
  const char* body_start = state.buffer.data() + headerEnd + 4;
  const_cast<HttpRequest&>(req).body = {body_start, contentLen};
  consumedBytes = totalNeeded;
  return true;
}

bool HttpServer::decodeChunkedBody(int fd, ConnStateInternal& state, const HttpRequest& req, std::size_t headerEnd,
                                   bool expectContinue, bool& closeConn, size_t& consumedBytes) {
  if (expectContinue) {
    queueData(fd, state, http::HTTP11_100_CONTINUE.data(), http::HTTP11_100_CONTINUE.size());
  }
  size_t pos = headerEnd + 4;
  string decodedBody;
  decodedBody.reserve(1024);
  bool needMore = false;
  static constexpr std::string_view kCRLF = "\r\n";
  while (true) {
    auto lineEndIt = std::search(state.buffer.begin() + pos, state.buffer.end(), kCRLF.begin(), kCRLF.end());
    if (lineEndIt == state.buffer.end()) {
      needMore = true;
      break;
    }
    std::string_view sizeLine(state.buffer.data() + pos, lineEndIt);
    size_t chunkSize = 0;
    for (char ch : sizeLine) {
      if (ch == ';') {
        break;
      }
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
    pos = (lineEndIt - state.buffer.data()) + 2;
    if (chunkSize > _config.maxBodyBytes) {
      string err;
      buildSimpleError(err, 413, http::ReasonPayloadTooLarge, _cachedDate, true);
      queueData(fd, state, err.data(), err.size());
      if (_parserErrCb) {
        _parserErrCb(ParserError::PayloadTooLarge);
      }
      closeConn = true;
      return false;
    }
    if (state.buffer.size() < pos + chunkSize + 2) {
      needMore = true;
      break;
    }
    if (chunkSize == 0) {
      if (state.buffer.size() < pos + 2) {
        needMore = true;
        break;
      }
      if (std::memcmp(state.buffer.data() + pos, kCRLF.data(), 2) != 0) {
        needMore = true;
        break;
      }
      pos += 2;
      break;
    }
    decodedBody.append(state.buffer, pos, chunkSize);
    if (decodedBody.size() > _config.maxBodyBytes) {
      string err;
      buildSimpleError(err, 413, http::ReasonPayloadTooLarge, _cachedDate, true);
      queueData(fd, state, err.data(), err.size());
      if (_parserErrCb) {
        _parserErrCb(ParserError::PayloadTooLarge);
      }
      closeConn = true;
      return false;
    }
    pos += chunkSize;
    if (std::memcmp(state.buffer.data() + pos, kCRLF.data(), 2) != 0) {
      needMore = true;
      break;
    }
    pos += 2;
  }
  if (needMore) {
    return false;
  }
  state.bodyStorage.assign(decodedBody.data(), decodedBody.size());
  const_cast<HttpRequest&>(req).body = std::string_view(state.bodyStorage);
  consumedBytes = pos;
  return true;
}

void HttpServer::finalizeAndSendResponse(int fd, ConnStateInternal& state, HttpRequest& req, HttpResponse& resp,
                                         size_t consumedBytes, bool& closeConn) {
  ++state.requestsServed;
  bool keepAlive = false;
  if (_config.enableKeepAlive) {
    if (req.version == http::HTTP11) {
      keepAlive = true;
    } else if (req.version == http::HTTP10) {
      keepAlive = false;
    }
    if (std::string_view connVal = req.findHeader(http::Connection); !connVal.empty()) {
      if (CaseInsensitiveEqual(connVal, http::close)) {
        keepAlive = false;
      } else if (CaseInsensitiveEqual(connVal, http::keepalive)) {
        keepAlive = true;
      }
    }
  }
  if (state.requestsServed >= _config.maxRequestsPerConnection) {
    keepAlive = false;
  }
  std::string_view body = resp.body;
  auto header = resp.buildHead(req.version, _cachedDate, keepAlive, body.size());
  if (req.method == http::HEAD) {
    queueData(fd, state, header.data(), header.size());
  } else {
    ::iovec iov[2];
    iov[0].iov_base = const_cast<char*>(header.data());
    iov[0].iov_len = header.size();
    iov[1].iov_base = const_cast<char*>(body.data());
    iov[1].iov_len = body.size();
    queueVec(fd, state, iov, 2);
  }
  if (consumedBytes > 0) {
    state.buffer.erase_front(consumedBytes);
  }
  if (!keepAlive) {
    closeConn = true;
  }
}

void HttpServer::handleReadableClient(int fd) {
  auto itState = _connStates.find(fd);
  if (itState == _connStates.end()) {
    return;  // stale
  }
  ConnStateInternal& state = itState->second;
  state.lastActivity = std::chrono::steady_clock::now();
  bool closeCnx = false;
  while (true) {
    state.buffer.ensureAvailableCapacity(4096);
    ssize_t count = ::read(fd, state.buffer.data() + state.buffer.size(), 4096);
    if (count < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      std::perror("read");
      closeCnx = true;
      break;
    }
    if (count == 0) {
      closeCnx = true;
      break;
    }
    state.buffer.setSize(state.buffer.size() + count);
    if (state.buffer.size() > _config.maxHeaderBytes + _config.maxBodyBytes) {
      closeCnx = true;
      break;
    }
    if (processRequestsOnConnection(fd, state)) {
      closeCnx = true;
      break;
    }
  }
  if (closeCnx) {
    closeConnection(fd);
  }
}

bool HttpServer::queueData(int fd, ConnStateInternal& state, const char* data, size_t len) {
  if (len == 0) {
    return true;
  }
  // If no pending data, try immediate write
  if (state.outBuffer.empty()) {
    ssize_t written = ::write(fd, data, len);
    if (written == static_cast<ssize_t>(len)) {
      // Count bytes as "queued" logically (attempted for send) even if fully written immediately.
      _stats.totalBytesQueued += static_cast<uint64_t>(len);
      _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
      return true;  // fully sent
    }
    if (written >= 0) {
      _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
      state.outBuffer.append(data + written, data + len);
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        state.outBuffer.append(data, data + len);
      } else {
        state.shouldClose = true;
        return false;
      }
    }
  } else {
    state.outBuffer.append(data, data + len);
  }
  _stats.totalBytesQueued += static_cast<uint64_t>(len);
  _stats.maxConnectionOutboundBuffer = std::max(_stats.maxConnectionOutboundBuffer, state.outBuffer.size());
  // Register EPOLLOUT if not already
  if (state.outBuffer.size() > _config.maxOutboundBufferBytes) {
    state.shouldClose = true;  // exceed limit, will close after flush attempt
  }
  if (!state.waitingWritable && _loop) {
    if (_loop->mod(fd, EPOLLIN | EPOLLOUT | EPOLLET)) {
      state.waitingWritable = true;
      _stats.deferredWriteEvents++;
    }
  }
  return true;
}

bool HttpServer::queueVec(int fd, ConnStateInternal& state, const struct iovec* iov, int iovcnt) {
  // Fast path: coalesce into single contiguous append attempt
  size_t total = 0;
  for (int i = 0; i < iovcnt; ++i) {
    total += iov[i].iov_len;
  }
  if (total == 0) {
    return true;
  }
  if (state.outBuffer.empty()) {
    ssize_t written = ::writev(fd, iov, iovcnt);
    if (written == static_cast<ssize_t>(total)) {
      // Count bytes as logically queued (attempted) even if fully written immediately.
      _stats.totalBytesQueued += static_cast<uint64_t>(total);
      _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
      return true;
    }
    if (written >= 0) {
      _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
      size_t remaining = static_cast<size_t>(written);
      // append remainder pieces
      for (int i = 0; i < iovcnt; ++i) {
        const char* base = static_cast<const char*>(iov[i].iov_base);
        size_t len = iov[i].iov_len;
        if (remaining >= len) {
          remaining -= len;
          continue;
        }
        base += remaining;
        len -= remaining;
        state.outBuffer.append(base, base + len);
        for (int j = i + 1; j < iovcnt; ++j) {
          const char* b2 = static_cast<const char*>(iov[j].iov_base);
          state.outBuffer.append(b2, b2 + iov[j].iov_len);
        }
        break;
      }
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        for (int i = 0; i < iovcnt; ++i) {
          const char* base = static_cast<const char*>(iov[i].iov_base);
          state.outBuffer.append(base, base + iov[i].iov_len);
        }
      } else {
        state.shouldClose = true;
        return false;
      }
    }
  } else {
    for (int i = 0; i < iovcnt; ++i) {
      const char* base = static_cast<const char*>(iov[i].iov_base);
      state.outBuffer.append(base, base + iov[i].iov_len);
    }
  }
  _stats.totalBytesQueued += static_cast<uint64_t>(total);
  _stats.maxConnectionOutboundBuffer = std::max(_stats.maxConnectionOutboundBuffer, state.outBuffer.size());
  if (state.outBuffer.size() > _config.maxOutboundBufferBytes) {
    state.shouldClose = true;
  }
  if (!state.waitingWritable && _loop) {
    if (_loop->mod(fd, EPOLLIN | EPOLLOUT | EPOLLET)) {
      state.waitingWritable = true;
      _stats.deferredWriteEvents++;
    }
  }
  return true;
}

void HttpServer::flushOutbound(int fd, ConnStateInternal& state) {
  _stats.flushCycles++;
  while (!state.outBuffer.empty()) {
    ssize_t written = ::write(fd, state.outBuffer.data(), state.outBuffer.size());
    if (written > 0) {
      _stats.totalBytesWrittenFlush += static_cast<uint64_t>(written);
      if (static_cast<size_t>(written) == state.outBuffer.size()) {
        state.outBuffer.clear();
        break;
      }
      state.outBuffer.erase_front(static_cast<size_t>(written));
      continue;
    }
    if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      break;  // stop for now
    }
    // Hard error
    state.shouldClose = true;
    state.outBuffer.clear();
    break;
  }
  if (state.outBuffer.empty() && state.waitingWritable && _loop) {
    // remove EPOLLOUT interest
    if (_loop->mod(fd, EPOLLIN | EPOLLET)) {
      state.waitingWritable = false;
      if (state.shouldClose) {
        closeConnection(fd);
      }
    }
  }
}

void HttpServer::handleWritableClient(int fd, uint32_t /*ev*/) {
  auto it = _connStates.find(fd);
  if (it == _connStates.end()) {
    return;
  }
  flushOutbound(fd, it->second);
}

void HttpServer::eventLoop(Duration timeout) {
  if (!_loop) {
    return;
  }
  refreshCachedDate();
  sweepIdleConnections();
  _loop->poll(timeout, [&](int fd, uint32_t ev) {
    if (fd == _listenFd) {
      acceptNewConnections();
    } else {
      if (ev & EPOLLOUT) {
        handleWritableClient(fd, ev);
      }
      if (ev & EPOLLIN) {
        handleReadableClient(fd);
      }
    }
  });
}

}  // namespace aeronet
