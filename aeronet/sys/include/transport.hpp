#pragma once

#include <sys/types.h>
#include <unistd.h>

#include <cstddef>

namespace aeronet {

// Base transport abstraction; allows transparent TLS or plain socket IO.
class ITransport {
 public:
  virtual ~ITransport() = default;

  // Non-blocking read. Returns bytes read (>0), 0 on orderly close, -1 on EAGAIN/WANT (caller inspects want flags).
  virtual ssize_t read(char* buf, std::size_t len, bool& wantRead, bool& wantWrite) = 0;

  // Non-blocking write. Returns bytes written (>0), 0 no progress (treat like EAGAIN), -1 fatal error.
  virtual ssize_t write(const char* buf, std::size_t len, bool& wantRead, bool& wantWrite) = 0;

  [[nodiscard]] virtual bool handshakePending() const noexcept { return false; }
};

// Plain transport directly operates on a non-blocking fd.
class PlainTransport : public ITransport {
 public:
  explicit PlainTransport(int fd) : _fd(fd) {}

  ssize_t read(char* buf, std::size_t len, bool& wantRead, bool& wantWrite) override {
    wantRead = wantWrite = false;
    return ::read(_fd, buf, len);
  }

  ssize_t write(const char* buf, std::size_t len, bool& wantRead, bool& wantWrite) override {
    wantRead = wantWrite = false;
    return ::write(_fd, buf, len);
  }

 private:
  int _fd;
};

}  // namespace aeronet