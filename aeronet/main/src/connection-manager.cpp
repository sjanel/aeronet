#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "aeronet/server.hpp"
#include "event-loop.hpp"
#include "sys-utils.hpp"  // setNonBlocking, safeClose

namespace aeronet {

void HttpServer::sweepIdleConnections() {
  // Periodic maintenance of live connections: applies keep-alive timeout (if enabled) and
  // header read timeout (always, regardless of keep-alive enablement). The header read timeout
  // needs a periodic check because a client might send a partial request line then stall; no
  // further EPOLLIN events will arrive to trigger enforcement in handleReadableClient().
  auto now = std::chrono::steady_clock::now();
  for (auto it = _connStates.begin(); it != _connStates.end();) {
    bool closeThis = false;
    ConnStateInternal& st = it->second;
    // Keep-alive inactivity enforcement only if enabled.
    if (_config.enableKeepAlive) {
      if (st.shouldClose || (now - st.lastActivity) > _config.keepAliveTimeout) {
        closeThis = true;
      }
    } else if (st.shouldClose) {
      closeThis = true;
    }
    // Header read timeout: active if headerStart set and duration exceeded and no full request parsed yet.
    if (!closeThis && _config.headerReadTimeout.count() > 0 && st.headerStart.time_since_epoch().count() != 0) {
      if (now - st.headerStart > _config.headerReadTimeout) {
        closeThis = true;
      }
    }
    if (closeThis) {
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
    int client_fd = ::accept(_listenFd, reinterpret_cast<sockaddr*>(&in_addr), &in_len);
    if (client_fd < 0) {
      int savedErr = errno;  // capture errno before any other call
      if (savedErr == EAGAIN || savedErr == EWOULDBLOCK) {
        break;
      }
      log::error("accept failed: {}", std::strerror(savedErr));
      break;
    }
    if (setNonBlocking(client_fd) < 0) {
      int savedErr = errno;
      log::error("setNonBlocking failed fd={} err={}: {}", client_fd, savedErr, std::strerror(savedErr));
      ::close(client_fd);
      continue;
    }
    if (!_loop->add(client_fd, EPOLLIN | EPOLLET)) {
      int savedErr = errno;
      log::error("EventLoop add client failed fd={} err={}: {}", client_fd, savedErr, std::strerror(savedErr));
      ::close(client_fd);
      continue;
    }
    auto [itState, inserted] = _connStates.emplace(client_fd, ConnStateInternal{});
    ConnStateInternal* pst = &itState->second;
    while (true) {
      pst->buffer.ensureAvailableCapacity(4096);
      ssize_t bytesRead = ::read(client_fd, pst->buffer.data() + pst->buffer.size(), 4096);
      if (bytesRead > 0) {
        if (pst->headerStart.time_since_epoch().count() == 0) {
          pst->headerStart = std::chrono::steady_clock::now();
        }
        pst->buffer.resize_down(pst->buffer.size() + static_cast<std::size_t>(bytesRead));
        if (bytesRead < 4096) {
          break;
        }
        continue;
      }
      if (bytesRead == 0) {
        closeConnection(client_fd);
        pst = nullptr;
        break;
      }
      if (bytesRead == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        break;
      }
      closeConnection(client_fd);
      pst = nullptr;
      break;
    }
    if (pst == nullptr) {
      continue;
    }
    bool closeNow = processRequestsOnConnection(client_fd, *pst);
    if (closeNow && pst->outBuffer.empty()) {
      closeConnection(client_fd);
      pst = nullptr;
    }
  }
}

void HttpServer::closeConnection(int cfd) {
  if (_loop) {
    _loop->del(cfd);
  }
  safeClose(cfd, "connFd(closeConnection)");
  _connStates.erase(cfd);
}

void HttpServer::handleReadableClient(int fd) {
  auto itState = _connStates.find(fd);
  if (itState == _connStates.end()) {
    return;
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
      log::error("read failed: {}", std::strerror(errno));
      closeCnx = true;
      break;
    }
    if (count == 0) {
      closeCnx = true;
      break;
    }
    state.buffer.resize_down(state.buffer.size() + static_cast<std::size_t>(count));
    if (count > 0 && state.headerStart.time_since_epoch().count() == 0) {
      state.headerStart = std::chrono::steady_clock::now();
    }
    if (state.buffer.size() > _config.maxHeaderBytes + _config.maxBodyBytes) {
      closeCnx = true;
      break;
    }
    if (processRequestsOnConnection(fd, state)) {
      closeCnx = true;
      break;
    }
    // Header read timeout enforcement: if headers of current pending request are not complete yet
    // (heuristic: no full request parsed and buffer not empty) and duration exceeded -> close.
    if (_config.headerReadTimeout.count() > 0) {
      if (state.headerStart.time_since_epoch().count() != 0) {
        auto now = std::chrono::steady_clock::now();
        if (now - state.headerStart > _config.headerReadTimeout) {
          closeCnx = true;
          break;
        }
      }
    }
  }
  if (closeCnx) {
    closeConnection(fd);
  }
}

void HttpServer::handleWritableClient(int fd) {
  auto it = _connStates.find(fd);
  if (it == _connStates.end()) {
    return;
  }
  flushOutbound(fd, it->second);
}

}  // namespace aeronet
