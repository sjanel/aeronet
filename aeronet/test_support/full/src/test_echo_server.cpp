#include "aeronet/test_echo_server.hpp"

#ifdef AERONET_POSIX
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#elifdef AERONET_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/base-fd.hpp"
#include "aeronet/errno-throw.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/test_util.hpp"

namespace aeronet::test {

EchoServer::EchoServer(Socket sock, uint16_t pt, std::shared_ptr<std::atomic<bool>> sf, std::thread thr)
    : listenSocket(std::move(sock)), port(pt), stopFlag(std::move(sf)), echoThread(std::move(thr)) {}

EchoServer::~EchoServer() {
  if (stopFlag) {
    stopFlag->store(true, std::memory_order_release);
  }
  // Do NOT close listenSocket here — the raw fd captured by the echo thread could be
  // reused by the OS after close, causing accept() on a wrong socket.  The accept timeout
  // (250 ms) + stop flag is sufficient to wake the thread.  The Socket member destructor
  // runs after the thread is joined, so the fd stays valid until then.
  if (echoThread.joinable()) {
    echoThread.join();
  }
}

EchoServer startEchoServer() {
  Socket listenSock(Socket::Type::Stream);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // ephemeral
  if (::bind(listenSock.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ThrowSystemError("Error from ::bind");
  }
  if (::listen(listenSock.fd(), 1) == -1) {
    ThrowSystemError("Error from ::listen");
  }
  sockaddr_in actual{};
  socklen_t alen = sizeof(actual);
  if (::getsockname(listenSock.fd(), reinterpret_cast<sockaddr*>(&actual), &alen) != 0) {
    ThrowSystemError("Error from ::getsockname");
  }

  uint16_t port = ntohs(actual.sin_port);

  // Set a timeout on the listening socket so accept() doesn't block indefinitely.
  // This allows the echo thread to check the stop flag periodically.
  setRecvTimeout(listenSock.fd(), std::chrono::milliseconds{250});

  auto stopFlag = std::make_shared<std::atomic<bool>>(false);

  std::thread echoThread([fd = listenSock.fd(), stopFlag]() {
    try {
      BaseFd clientFd;
      while (true) {
        clientFd = BaseFd(::accept(fd, nullptr, nullptr));
        if (clientFd.fd() != kInvalidHandle) {
          break;
        }
        const auto err = LastSystemError();
        if (stopFlag->load(std::memory_order_acquire)) {
          return;
        }
#ifdef AERONET_POSIX
        if (err == error::kTimedOut || err == error::kWouldBlock) {
          continue;
        }
        // EBADF / EINVAL: socket was closed before accept (normal shutdown).
        if (err == EBADF || err == EINVAL) {
          return;
        }
#elifdef AERONET_WINDOWS
        if (err == error::kTimedOut) {
          continue;
        }
        // On Windows, closesocket() on the listen socket while accept() is blocked wakes
        // accept() with WSAEINTR (interrupted), WSAEINVAL (invalid socket), or WSAENOTSOCK
        // (socket already closed). Treat all of these as a clean server shutdown.
        if (err == WSAEINTR || err == WSAEINVAL || err == WSAENOTSOCK) {
          return;
        }
#endif
        // ECONNABORTED: connection was reset before accept
        // (can occur on macOS/Windows under load or during test teardown).
        if (err == error::kConnectionAborted) {
          return;
        }
        return;  // unexpected error — exit gracefully instead of throwing
      }
      // Break out after a bounded idle period with no traffic.
      setRecvTimeout(clientFd.fd(), std::chrono::milliseconds{250});
      const auto maxIdle = std::chrono::seconds{3};
      auto idleDeadline = std::chrono::steady_clock::now() + maxIdle;
      char buf[16384];

      while (true) {
        const auto recvBytes = ::recv(clientFd.fd(), buf, static_cast<int>(sizeof(buf)), 0);
        if (recvBytes == -1) {
          const auto err = LastSystemError();
#ifdef AERONET_WINDOWS
          if (err == error::kTimedOut) {
            if (stopFlag->load(std::memory_order_acquire) || std::chrono::steady_clock::now() >= idleDeadline) {
              break;
            }
            continue;
          }
          if (err == error::kConnectionReset || err == WSAENOTCONN) {
            break;
          }
#else
          if (err == error::kInterrupted) {
            continue;
          }
          if (err == error::kTimedOut || err == error::kWouldBlock) {
            if (stopFlag->load(std::memory_order_acquire) || std::chrono::steady_clock::now() >= idleDeadline) {
              break;
            }
            continue;
          }
          if (err == error::kConnectionReset || err == ENOTCONN || err == EBADF) {
            break;
          }
#endif
          break;  // unexpected error — exit gracefully instead of throwing
        }
        if (recvBytes == 0) {
          break;
        }
        idleDeadline = std::chrono::steady_clock::now() + maxIdle;

        try {
          sendAll(clientFd.fd(), std::string_view(buf, static_cast<std::size_t>(recvBytes)));
          // Reset idle deadline after successful send — time spent blocked in
          // ::send() (due to TCP backpressure on small-buffer platforms like
          // macOS/Windows) is NOT idle time and must not count towards maxIdle.
          idleDeadline = std::chrono::steady_clock::now() + maxIdle;
        } catch (const std::exception& ex) {
          log::error("Exception in echo server thread while sending data: {}", ex.what());
          break;
        }
      }
    } catch (const std::exception& ex) {
      log::error("Exception in echo server thread: {}", ex.what());
    }
  });

  return EchoServer{std::move(listenSock), port, stopFlag, std::move(echoThread)};
}

}  // namespace aeronet::test