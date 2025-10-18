#include "tcp-connector.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string_view>

namespace aeronet {

ConnectResult ConnectTCP(char* buf, std::string_view host, std::string_view port, int family) {
  addrinfo* res = nullptr;

  ConnectResult connectResult;

  int gai;
  {
    struct CharReplacer {
      explicit CharReplacer(char* pos) : ch(*pos), pos(pos) { *pos = '\0'; }

      CharReplacer(const CharReplacer&) = delete;
      CharReplacer(CharReplacer&&) = delete;
      CharReplacer& operator=(const CharReplacer&) = delete;
      CharReplacer& operator=(CharReplacer&&) = delete;

      ~CharReplacer() { *pos = ch; }

      char ch;
      char* pos;
    };

    CharReplacer hostReplacer(buf + (host.data() + host.size() - buf));
    CharReplacer portReplacer(buf + (port.data() + port.size() - buf));

    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;

    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    gai = ::getaddrinfo(host.data(), port.data(), &hints, &res);
  }
  std::unique_ptr<addrinfo, void (*)(addrinfo*)> resRAII(res, &::freeaddrinfo);

  if (gai != 0 || res == nullptr) {
    // avoid depending on a logging helper here; getaddrinfo error is enough
    connectResult.failure = true;
    return connectResult;
  }

  for (addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
    const int socktype = rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC;

    connectResult.cnx = Connection(BaseFd(::socket(rp->ai_family, socktype, rp->ai_protocol)));
    if (!connectResult.cnx.isOpened()) {
      int saved = errno;
      if (saved == EMFILE || saved == ENFILE) {
        connectResult.failure = true;
        return connectResult;
      }
      continue;
    }

    if (::connect(connectResult.cnx.fd(), rp->ai_addr, rp->ai_addrlen) == 0) {
      // connected immediately
      return connectResult;
    }

    if (errno == EINPROGRESS) {
      // non-blocking connect in progress
      connectResult.connectPending = true;
      return connectResult;
    }
  }
  connectResult.failure = true;
  return connectResult;
}

}  // namespace aeronet
