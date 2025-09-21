#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>

#include "aeronet/event-loop.hpp"
#include "aeronet/server.hpp"
#include "sys-utils.hpp"  // setNonBlocking, safeClose

namespace aeronet {

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
    int client_fd = ::accept(_listenFd, reinterpret_cast<sockaddr*>(&in_addr), &in_len);
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
      continue;
    }
    auto [itState, inserted] = _connStates.emplace(client_fd, ConnStateInternal{});
    ConnStateInternal* pst = &itState->second;
    while (true) {
      pst->buffer.ensureAvailableCapacity(4096);
      ssize_t bytesRead = ::read(client_fd, pst->buffer.data() + pst->buffer.size(), 4096);
      if (bytesRead > 0) {
        pst->buffer.setSize(pst->buffer.size() + bytesRead);
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

void HttpServer::handleWritableClient(int fd, uint32_t /*ev*/) {
  auto it = _connStates.find(fd);
  if (it == _connStates.end()) {
    return;
  }
  flushOutbound(fd, it->second);
}

}  // namespace aeronet
