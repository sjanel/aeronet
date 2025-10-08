#pragma once

#include "socket.hpp"

namespace aeronet {

// Simple RAII class wrapping a Connection accepted on a blocking socket.
class Connection {
 public:
  explicit Connection(const Socket &socket);

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