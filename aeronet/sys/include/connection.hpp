#pragma once

#include "base-fd.hpp"
#include "socket.hpp"

namespace aeronet {

// Simple RAII class wrapping a Connection accepted on a blocking socket.
class Connection {
 public:
  Connection() noexcept = default;

  explicit Connection(const Socket &socket);
  // Construct a Connection that takes ownership of an existing fd wrapped in BaseFd.
  explicit Connection(BaseFd &&bd) noexcept;

  [[nodiscard]] int fd() const noexcept { return _baseFd.fd(); }

  [[nodiscard]] bool isOpened() const noexcept { return _baseFd.isOpened(); }

  void close() noexcept { _baseFd.close(); }

  bool operator==(const Connection &) const noexcept = default;

  // This operator int is used in the connections map in HttpServer. This allows usage of transparent look-ups
  // from Fd received from the event loop.
  operator int() const noexcept { return _baseFd.fd(); }

 private:
  BaseFd _baseFd;
};

}  // namespace aeronet