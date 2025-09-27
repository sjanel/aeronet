#include <sys/uio.h>  // for ::iovec NOLINT(misc-include-cleaner)

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server.hpp"
#include "http-constants.hpp"
#include "http-response-build.hpp"
#include "log.hpp"
#include "string-equal-ignore-case.hpp"

namespace aeronet {
void HttpServer::finalizeAndSendResponse(int fd, ConnectionState& state, HttpRequest& req, HttpResponse& resp,
                                         std::size_t consumedBytes, std::chrono::steady_clock::time_point reqStart,
                                         bool& closeConn) {
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
  // TODO: make it zero copy (without headers RawChars)
  auto header = http::buildHead(resp, req.version, std::string_view(_cachedDate), keepAlive);
  if (req.method == http::HEAD) {
    // For HEAD we only send headers (no body). Any pending bytes in outBuffer belong to prior responses and will
    // naturally flush before these headers on the wire (HTTP pipelining semantics). No special handling needed.
    queueData(fd, state, header);
  } else {
    std::string_view dataStrs[2]{header, resp.body()};
    queueVec(fd, state, dataStrs);
  }
  state.buffer.erase_front(consumedBytes);
  if (!keepAlive) {
    closeConn = true;
  }
  if (_metricsCb) {
    RequestMetrics metrics;
    metrics.method = req.method;
    metrics.target = req.target;
    metrics.status = resp.statusCode();
    metrics.bytesIn = req.body.size();
    metrics.reusedConnection = state.requestsServed > 0;
    metrics.duration = std::chrono::steady_clock::now() - reqStart;
    _metricsCb(metrics);
  }
}

bool HttpServer::queueData(int fd, ConnectionState& state, std::string_view data) {
  if (state.outBuffer.empty()) {
    bool wantR = false;
    bool wantW = false;
    auto written = transportWrite(fd, state, data, wantR, wantW);
    state.tlsWantRead = wantR;
    state.tlsWantWrite = wantW;
    if (!state.tlsEstablished && state.transport && !state.transport->handshakePending()) {
      state.tlsEstablished = true;
    }
    if (written > 0) {
      if (std::cmp_equal(written, data.size())) {
        _stats.totalBytesQueued += static_cast<uint64_t>(data.size());
        _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
        // If TLS wants more write (handshake) and no buffered data, ensure EPOLLOUT
        if (wantW && !state.waitingWritable) {
          if (!HttpServer::ModWithCloseOnFailure(_loop, fd, EPOLLIN | EPOLLOUT | EPOLLET, state,
                                                 "enable writable immediate write path", _stats)) {
            return false;
          }
          state.waitingWritable = true;
          ++_stats.deferredWriteEvents;
        }
        return true;
      }
      // partial write
      _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
      state.outBuffer.append(data.data() + written, data.data() + data.size());
    } else if (written == 0 || (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
      // no progress but non fatal
      state.outBuffer.append(data);
    } else {  // fatal
      state.shouldClose = true;
      return false;
    }
  } else {
    state.outBuffer.append(data);
  }
  _stats.totalBytesQueued += static_cast<uint64_t>(data.size());
  _stats.maxConnectionOutboundBuffer = std::max(_stats.maxConnectionOutboundBuffer, state.outBuffer.size());
  if (state.outBuffer.size() > _config.maxOutboundBufferBytes) {
    state.shouldClose = true;
  }
  if (!state.waitingWritable) {
    if (HttpServer::ModWithCloseOnFailure(_loop, fd, EPOLLIN | EPOLLOUT | EPOLLET, state,
                                          "enable writable buffered path", _stats)) {
      state.waitingWritable = true;
      ++_stats.deferredWriteEvents;
    }
  }
  return true;
}

bool HttpServer::queueVec(int fd, ConnectionState& state, std::span<const std::string_view> dataStrs) {
  std::size_t total = std::accumulate(dataStrs.begin(), dataStrs.end(), static_cast<std::size_t>(0),
                                      [](std::size_t sum, const std::string_view& sv) { return sum + sv.size(); });
  if (state.outBuffer.empty()) {
    // Attempt to write iov chunks sequentially via transportWrite.
    std::size_t advanced = 0;
    for (std::size_t dataPos1 = 0; dataPos1 < dataStrs.size(); ++dataPos1) {
      std::string_view baseView = dataStrs[dataPos1];
      while (!baseView.empty()) {
        bool wantR = false;
        bool wantW = false;
        auto bytesWritten = transportWrite(fd, state, baseView, wantR, wantW);
        state.tlsWantRead = wantR;
        state.tlsWantWrite = wantW;
        if (!state.tlsEstablished && state.transport && !state.transport->handshakePending()) {
          state.tlsEstablished = true;
        }
        if (bytesWritten > 0) {
          _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(bytesWritten);
          advanced += static_cast<std::size_t>(bytesWritten);
          baseView.remove_prefix(static_cast<std::string_view::size_type>(bytesWritten));
          if (wantW && !state.waitingWritable) {
            if (!HttpServer::ModWithCloseOnFailure(_loop, fd, EPOLLIN | EPOLLOUT | EPOLLET, state,
                                                   "enable writable queueVec handshake path", _stats)) {
              return false;
            }
            state.waitingWritable = true;
            ++_stats.deferredWriteEvents;
          }
          continue;
        }
        if (bytesWritten == 0 || (bytesWritten == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
          // buffer remaining of this iov and rest
          state.outBuffer.append(baseView);
          for (std::size_t dataPos2 = dataPos1 + 1; dataPos2 < dataStrs.size(); ++dataPos2) {
            state.outBuffer.append(dataStrs[dataPos2]);
          }
          break;
        }
        // fatal
        state.shouldClose = true;
        return false;
      }
      if (!state.outBuffer.empty()) {
        break;  // rest already buffered
      }
    }
    if (advanced == total && state.outBuffer.empty()) {
      _stats.totalBytesQueued += static_cast<uint64_t>(total);
      return true;
    }
  } else {
    for (auto data : dataStrs) {
      state.outBuffer.append(data);
    }
  }
  _stats.totalBytesQueued += static_cast<uint64_t>(total);
  _stats.maxConnectionOutboundBuffer = std::max<decltype(_stats.maxConnectionOutboundBuffer)>(
      _stats.maxConnectionOutboundBuffer, state.outBuffer.size());
  if (state.outBuffer.size() > _config.maxOutboundBufferBytes) {
    state.shouldClose = true;
  }
  if (!state.waitingWritable) {
    if (!HttpServer::ModWithCloseOnFailure(_loop, fd, EPOLLIN | EPOLLOUT | EPOLLET, state,
                                           "enable writable queueVec buffered path", _stats)) {
      return false;
    }
    state.waitingWritable = true;
    ++_stats.deferredWriteEvents;
  }
  return true;
}

void HttpServer::flushOutbound(int fd, ConnectionState& state) {
  ++_stats.flushCycles;
  bool lastWantWrite = false;
  while (!state.outBuffer.empty()) {
    bool wantR = false;
    bool wantW = false;
    auto written = transportWrite(fd, state, state.outBuffer, wantR, wantW);
    lastWantWrite = wantW;
    state.tlsWantRead = wantR;
    state.tlsWantWrite = wantW;
    if (!state.tlsEstablished && state.transport && !state.transport->handshakePending()) {
      state.tlsEstablished = true;
    }
    if (written > 0) {
      _stats.totalBytesWrittenFlush += static_cast<uint64_t>(written);
      if (std::cmp_equal(written, state.outBuffer.size())) {
        state.outBuffer.clear();
        break;
      }
      state.outBuffer.erase_front(static_cast<std::size_t>(written));
      continue;
    }
    if (written == 0 || (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
      // Need to wait for socket writable again (TLS handshake or congestion)
      break;
    }
    int savedErr = errno;
    log::error("send/transportWrite failed fd={} errno={} msg={}", fd, savedErr, std::strerror(savedErr));
    state.shouldClose = true;
    state.outBuffer.clear();
    break;
  }
  // Determine if we can drop EPOLLOUT: only when no buffered data AND no handshake wantWrite pending.
  if (state.outBuffer.empty() && state.waitingWritable) {
    bool keepWritable = false;
    if (!state.tlsEstablished && state.transport && state.transport->handshakePending()) {
      keepWritable = true;
    }
    if (!keepWritable) {
      if (HttpServer::ModWithCloseOnFailure(_loop, fd, EPOLLIN | EPOLLET, state,
                                            "disable writable flushOutbound drop EPOLLOUT", _stats)) {
        state.waitingWritable = false;
        if (state.shouldClose) {
          closeConnection(fd);
        }
      }
    }
  }
  // Clear writable interest if no buffered data and transport no longer needs write progress.
  // (We do not call handshakePending() here because ConnStateInternal does not expose it; transport has that.)
  if (state.outBuffer.empty()) {
    bool transportNeedsWrite = (!state.tlsEstablished && (state.tlsWantWrite || lastWantWrite));
    if (transportNeedsWrite) {
      if (!state.waitingWritable) {
        if (!HttpServer::ModWithCloseOnFailure(_loop, fd, EPOLLIN | EPOLLOUT | EPOLLET, state,
                                               "enable writable flushOutbound transportNeedsWrite", _stats)) {
          return;  // failure logged
        }
        state.waitingWritable = true;
      }
    } else if (state.waitingWritable) {
      state.waitingWritable = false;
      HttpServer::ModWithCloseOnFailure(_loop, fd, EPOLLIN | EPOLLET, state,
                                        "disable writable flushOutbound transport no longer needs", _stats);
    }
  }
}

}  // namespace aeronet
