#include "aeronet/tcp-connector.hpp"

#include <type_traits>

#ifdef AERONET_WINDOWS
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#endif

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string_view>

#include "aeronet/base-fd.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/socket-ops.hpp"  // GetSocketError (and SetNonBlocking / SetCloseOnExec off-Linux)
#include "aeronet/system-error-message.hpp"
#include "aeronet/system-error.hpp"

namespace aeronet {

namespace {

// Outcome of waiting for a single non-blocking connect() to resolve.
enum class ConnectWait : uint8_t { Connected, Failed, TimedOut };

// Block until a pending non-blocking connect on `fd` completes, fails, or the deadline elapses.
// SO_ERROR disambiguates a writable-but-refused socket (POLLOUT is also raised on connect failure).
ConnectWait WaitForConnectCompletion(NativeHandle fd, std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return ConnectWait::TimedOut;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int timeoutMs =
        remaining > static_cast<std::remove_const_t<decltype(remaining)>>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(remaining);
#ifdef AERONET_WINDOWS
    WSAPOLLFD pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int pr = ::WSAPoll(&pfd, 1, timeoutMs);
#else
    pollfd pfd{};  // NOLINT(misc-include-cleaner)
    pfd.fd = fd;
    pfd.events = POLLOUT;                       // NOLINT(misc-include-cleaner)
    const int pr = ::poll(&pfd, 1, timeoutMs);  // NOLINT(misc-include-cleaner)
#endif
    if (pr < 0) {
      if (LastSystemError() == error::kInterrupted) {
        continue;  // signal interruption: re-arm with the remaining budget
      }
      return ConnectWait::Failed;
    }
    if (pr == 0) {
      return ConnectWait::TimedOut;
    }
    return GetSocketError(fd) == 0 ? ConnectWait::Connected : ConnectWait::Failed;
  }
}

class CharReplacer {
 public:
  explicit CharReplacer(char* pos) : ch(*pos), pos(pos) { *pos = '\0'; }

  CharReplacer(const CharReplacer&) = delete;
  CharReplacer(CharReplacer&&) = delete;
  CharReplacer& operator=(const CharReplacer&) = delete;
  CharReplacer& operator=(CharReplacer&&) = delete;

  ~CharReplacer() { *pos = ch; }

 private:
  char ch;
  char* pos;
};
}  // namespace

ConnectResult ConnectTCP(std::span<char> host, std::span<char> port, int family, int connectTimeoutMs) {
#ifdef AERONET_WINDOWS
  EnsureWinsockInitialized();
#endif
  addrinfo* res = nullptr;

  int gai;
  {
    CharReplacer hostReplacer(host.data() + host.size());
    CharReplacer portReplacer(port.data() + port.size());

    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;

    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    gai = ::getaddrinfo(host.data(), port.data(), &hints, &res);
  }
  std::unique_ptr<addrinfo, void (*)(addrinfo*)> resRAII(res, &::freeaddrinfo);
  ConnectResult connectResult;

  if (gai != 0) [[unlikely]] {
    log::error("ConnectTCP: getaddrinfo('{}', '{}') failed: {}", std::string_view(host), std::string_view(port),
               ::gai_strerror(gai));
    connectResult.failure = true;
    return connectResult;
  }

  // When connectTimeoutMs > 0 the caller wants a fully-established socket: each pending connect is driven
  // to completion here so a failed candidate (e.g. localhost's ::1 against an IPv4-only server) falls back
  // to the next one. The budget is shared across all candidates.
  const bool blockingFallback = connectTimeoutMs > 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(connectTimeoutMs);

  for (addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
#ifdef AERONET_LINUX
    const int socktype = rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC;
    connectResult.cnx = Connection(BaseFd(::socket(rp->ai_family, socktype, rp->ai_protocol)));
#else
    connectResult.cnx = Connection(BaseFd(::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol)));
    if (connectResult.cnx) {
      SetNonBlocking(connectResult.cnx.fd());
#ifdef AERONET_POSIX
      SetCloseOnExec(connectResult.cnx.fd());
#endif
    }
#endif
    if (!connectResult.cnx) [[unlikely]] {
      const int saved = LastSystemError();
      log::error("ConnectTCP: socket() failed for addrinfo entry (family={}, socktype={}, protocol={}): err={}, msg={}",
                 rp->ai_family, rp->ai_socktype, rp->ai_protocol, saved, SystemErrorMessage(saved));
      if (saved == error::kTooManyFiles || saved == ENFILE) {
        break;  // no point in continuing
      }
      continue;
    }

#ifdef AERONET_WINDOWS
    if (::connect(connectResult.cnx.fd(), rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) {
#else
    if (::connect(connectResult.cnx.fd(), rp->ai_addr, rp->ai_addrlen) == 0) {
#endif
      // connected immediately
      return connectResult;
    }

    const int connectErr = LastSystemError();
    // Non-blocking connect started -> completion will be signalled via poll/epoll/WSAPoll
    switch (connectErr) {
      case error::kWouldBlock:
        // Windows returns WSAEWOULDBLOCK (10035) from connect() on a non-blocking socket;
        // Linux/macOS return EINPROGRESS instead. Both mean "connection in progress".
        [[fallthrough]];
      case error::kInProgress:
        [[fallthrough]];
      case error::kAlready:
        // EALREADY: a previous non-blocking connect is already in progress on this socket
        if (!blockingFallback) {
          connectResult.connectPending = true;
          return connectResult;  // caller drives completion via its own event loop
        }
        switch (WaitForConnectCompletion(connectResult.cnx.fd(), deadline)) {
          case ConnectWait::Connected:
            return connectResult;  // connectPending stays false: socket is established
          case ConnectWait::Failed:
            continue;  // close this socket (reassigned next iteration) and try the next candidate
          case ConnectWait::TimedOut:
            connectResult.cnx = {};
            connectResult.failure = true;
            return connectResult;
        }
        break;
      case error::kInterrupted:
        // Interrupted system call; treat as transient and try next address
        continue;
      default:
        log::error(
            "ConnectTCP: connect() failed for addrinfo entry (family={}, socktype={}, protocol={}): err={}, msg={}",
            rp->ai_family, rp->ai_socktype, rp->ai_protocol, connectErr, SystemErrorMessage(connectErr));
        break;
    }
  }
  // No candidate connected: never hand back a half-open / last-attempted socket on failure.
  connectResult.cnx = {};
  connectResult.failure = true;
  return connectResult;
}

}  // namespace aeronet
