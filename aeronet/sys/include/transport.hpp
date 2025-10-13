#pragma once

#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace aeronet {

// Indicates what the transport layer needs to proceed after a non-blocking I/O operation returns EAGAIN/WANT.
enum class TransportWant : uint8_t {
  None,       // No special action needed (operation completed or fatal error)
  ReadReady,  // Need socket readable before operation can proceed (SSL_ERROR_WANT_READ)
  WriteReady  // Need socket writable before operation can proceed (SSL_ERROR_WANT_WRITE)
};

// Base transport abstraction; allows transparent TLS or plain socket IO.
class ITransport {
 public:
  virtual ~ITransport() = default;

  // Non-blocking read. Returns bytes read (>0), 0 on orderly close, -1 on EAGAIN/WANT (caller inspects want).
  // want: indicates whether socket needs to be readable or writable for operation to proceed.
  virtual ssize_t read(char* buf, std::size_t len, TransportWant& want) = 0;

  // Non-blocking write. Returns bytes written (>0), 0 no progress (treat like EAGAIN), -1 fatal error.
  // want: indicates whether socket needs to be readable or writable for operation to proceed.
  virtual ssize_t write(std::string_view data, TransportWant& want) = 0;

  [[nodiscard]] virtual bool handshakePending() const noexcept { return false; }
};

// Plain transport directly operates on a non-blocking fd.
class PlainTransport : public ITransport {
 public:
  explicit PlainTransport(int fd) : _fd(fd) {}

  ssize_t read(char* buf, std::size_t len, TransportWant& want) override {
    want = TransportWant::None;
    return ::read(_fd, buf, len);
  }

  ssize_t write(std::string_view data, TransportWant& want) override {
    want = TransportWant::None;

    ssize_t total = 0;

    while (std::cmp_less(total, data.size())) {
      ssize_t res = ::write(_fd, data.data() + total, data.size() - static_cast<std::size_t>(total));

      if (res > 0) {
        total += res;
      } else if (res == -1 && errno == EINTR) {
        // Interrupted by signal, retry immediately
        continue;
      } else if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // Kernel send buffer full — caller should wait for writable event
        want = TransportWant::WriteReady;
        break;
      } else {
        // Fatal error (ECONNRESET, EPIPE, etc.)
        return -1;
      }
    }

    // Return how much we actually wrote.
    // If total == 0 and wantWrite==true, treat it like EAGAIN/no progress.
    return total;
  }

 private:
  int _fd;
};

}  // namespace aeronet