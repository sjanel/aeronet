#include <sys/epoll.h>  // EPOLL* constants
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

#include "aeronet/server.hpp"
#include "event-loop.hpp"  // for EventLoop methods
#include "http-constants.hpp"
#include "http-response-build.hpp"
#include "log.hpp"
#include "string-equal-ignore-case.hpp"  // for CaseInsensitiveEqual used indirectly in finalize logic

namespace aeronet {

void HttpServer::finalizeAndSendResponse(int fd, ConnStateInternal& state, HttpRequest& req, HttpResponse& resp,
                                         std::size_t consumedBytes, bool& closeConn) {
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
  auto header = http::buildHead(resp, req.version, std::string_view(_cachedDate), keepAlive, body.size());
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

bool HttpServer::queueData(int fd, ConnStateInternal& state, const char* data, std::size_t len) {
  if (state.outBuffer.empty()) {
    ssize_t written = ::send(fd, data, len, MSG_NOSIGNAL);
    if (written == static_cast<ssize_t>(len)) {
      _stats.totalBytesQueued += static_cast<uint64_t>(len);
      _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
      return true;
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
  if (state.outBuffer.size() > _config.maxOutboundBufferBytes) {
    state.shouldClose = true;
  }
  if (!state.waitingWritable && _loop) {
    if (_loop->mod(fd, EPOLLIN | EPOLLOUT | EPOLLET)) {
      state.waitingWritable = true;
      _stats.deferredWriteEvents++;
    } else {
      int savedErr = errno;
      log::error("epoll_ctl MOD (enable writable) failed fd={} errno={} msg={}", fd, savedErr, std::strerror(savedErr));
      state.shouldClose = true;  // can't monitor for writability reliably
    }
  }
  return true;
}

bool HttpServer::queueVec(int fd, ConnStateInternal& state, const struct iovec* iov, int iovcnt) {
  std::size_t total = 0;
  for (int i = 0; i < iovcnt; ++i) {
    total += iov[i].iov_len;
  }
  if (total == 0) {
    return true;
  }
  if (state.outBuffer.empty()) {
    struct msghdr msg{};
    msg.msg_iov = const_cast<struct iovec*>(iov);
    msg.msg_iovlen = static_cast<std::size_t>(iovcnt);
    ssize_t written = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
    if (written == static_cast<ssize_t>(total)) {
      _stats.totalBytesQueued += static_cast<uint64_t>(total);
      _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
      return true;
    }
    if (written >= 0) {
      _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
      std::size_t remaining = static_cast<std::size_t>(written);
      for (int i = 0; i < iovcnt; ++i) {
        const char* base = static_cast<const char*>(iov[i].iov_base);
        std::size_t len = iov[i].iov_len;
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
    ssize_t written = ::send(fd, state.outBuffer.data(), state.outBuffer.size(), MSG_NOSIGNAL);
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
      break;
    }
    int savedErr = errno;
    log::error("send failed fd={} errno={} msg={}", fd, savedErr, std::strerror(savedErr));
    state.shouldClose = true;
    state.outBuffer.clear();
    break;
  }
  if (state.outBuffer.empty() && state.waitingWritable && _loop) {
    if (_loop->mod(fd, EPOLLIN | EPOLLET)) {
      state.waitingWritable = false;
      if (state.shouldClose) {
        closeConnection(fd);
      }
    } else {
      int savedErr = errno;
      log::error("epoll_ctl MOD (disable writable) failed fd={} errno={} msg={}", fd, savedErr,
                 std::strerror(savedErr));
      state.shouldClose = true;
    }
  }
}

}  // namespace aeronet
