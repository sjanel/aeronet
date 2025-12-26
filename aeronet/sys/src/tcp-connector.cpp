#include "aeronet/tcp-connector.hpp"

#include <netdb.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>

#include "aeronet/base-fd.hpp"
#include "aeronet/log.hpp"

namespace aeronet {

namespace {
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

ConnectResult ConnectTCP(std::span<char> host, std::span<char> port, int family) {
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

  for (addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
    const int socktype = rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC;

    connectResult.cnx = Connection(BaseFd(::socket(rp->ai_family, socktype, rp->ai_protocol)));
    if (!connectResult.cnx) [[unlikely]] {
      const int saved = errno;
      log::error(
          "ConnectTCP: socket() failed for addrinfo entry (family={}, socktype={}, protocol={}): errno={}, msg={}",
          rp->ai_family, rp->ai_socktype, rp->ai_protocol, saved, std::strerror(saved));
      if (saved == EMFILE || saved == ENFILE) {
        break;  // no point in continuing
      }
      continue;
    }

    if (::connect(connectResult.cnx.fd(), rp->ai_addr, rp->ai_addrlen) == 0) {
      // connected immediately
      return connectResult;
    }

    const int connectErr = errno;
    // Non-blocking connect started -> completion will be signalled via poll/epoll
    switch (connectErr) {
      case EINPROGRESS:
        [[fallthrough]];
      case EALREADY:
        // EALREADY: a previous non-blocking connect is already in progress on this socket
        connectResult.connectPending = true;
        return connectResult;
      case EINTR:
        // EINTR: interrupted system call; treat as transient and try next address
        continue;
      default:
        log::error(
            "ConnectTCP: connect() failed for addrinfo entry (family={}, socktype={}, protocol={}): errno={}, msg={}",
            rp->ai_family, rp->ai_socktype, rp->ai_protocol, connectErr, std::strerror(connectErr));
        break;
    }
  }
  connectResult.failure = true;
  return connectResult;
}

}  // namespace aeronet
