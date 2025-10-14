#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/http-response-data.hpp"

namespace aeronet {

// Indicates what the transport layer needs to proceed after a non-blocking I/O operation returns EAGAIN/WANT.
enum class Transport : uint8_t {
  None,        // No special action needed (operation completed or fatal error)
  ReadReady,   // Need socket readable before operation can proceed (SSL_ERROR_WANT_READ)
  WriteReady,  // Need socket writable before operation can proceed (SSL_ERROR_WANT_WRITE)
  Error
};

// Base transport abstraction; allows transparent TLS or plain socket IO.
class ITransport {
 public:
  virtual ~ITransport() = default;

  // Non-blocking read. Returns bytes read (>0), 0 on orderly close, -1 on EAGAIN/WANT (caller inspects want).
  // want: indicates whether socket needs to be readable or writable for operation to proceed.
  virtual std::size_t read(char* buf, std::size_t len, Transport& want) = 0;

  // Non-blocking write. Returns the number of bytes written. If 0, check the want parameter.
  // want: indicates whether socket needs to be readable or writable for operation to proceed.
  virtual std::size_t write(std::string_view data, Transport& want) = 0;

  // Non-blocking write. Returns bytes written (>0), 0 no progress (treat like EAGAIN), -1 fatal error.
  // want: indicates whether socket needs to be readable or writable for operation to proceed.
  std::size_t write(const HttpResponseData& httpResponseData, Transport& want) {
    // First attempt to write the response head. Only if the head was fully
    // written do we proceed to write the body. This is important for TLS
    // transports where a write call may succeed and report a positive
    // "bytes written" value that is nevertheless smaller than the
    // requested buffer. In that partial-write case we must not start
    // sending the body bytes before the remaining head bytes have been
    // flushed, otherwise the client will see a corrupted/invalid response.
    std::size_t total = write(httpResponseData.firstBuffer(), want);
    if (want != Transport::None) {
      // Transport indicated it needs readiness or error — caller will retry.
      return total;
    }

    // Only continue to body if the head was fully consumed.
    if (total < httpResponseData.firstBuffer().size()) {
      return total;
    }

    auto bodyData = httpResponseData.secondBuffer();
    if (!bodyData.empty()) {
      total += write(bodyData, want);
    }
    return total;
  }

  [[nodiscard]] virtual bool handshakeDone() const noexcept { return true; }
};

// Plain transport directly operates on a non-blocking fd.
class PlainTransport : public ITransport {
 public:
  explicit PlainTransport(int fd) : _fd(fd) {}

  std::size_t read(char* buf, std::size_t len, Transport& want) override;

  std::size_t write(std::string_view data, Transport& want) override;

 private:
  int _fd;
};

}  // namespace aeronet