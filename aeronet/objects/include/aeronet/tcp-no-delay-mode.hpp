#pragma once

#include <cstdint>

namespace aeronet {

enum class TcpNoDelayMode : std::uint8_t {
  // Enabled when TLS is enabled (HTTP/1.1 and HTTP/2 over TLS), disabled otherwise (cleartext). OpenSSL writes each TLS
  // record on its own: with Nagle's algorithm, the last partial segment of a record waits for the acknowledgment of the
  // previous one (up to 40 ms with delayed acknowledgments), for the first response of each connection and for each
  // response of several records. A cleartext response usually takes a single write (head and body gathered).
  Auto,

  // Always disable TCP_NODELAY (enable Nagle).
  Disabled,

  // Always enable TCP_NODELAY (disable Nagle).
  Enabled,
};

}
