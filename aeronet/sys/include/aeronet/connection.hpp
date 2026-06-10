#pragma once

#ifdef AERONET_WINDOWS
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include <amc/type_traits.hpp>

#include "aeronet/base-fd.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/socket.hpp"

namespace aeronet {

// Simple RAII class wrapping a Connection accepted on a blocking socket.
class Connection {
 public:
  Connection() noexcept = default;

  Connection(const Socket& socket, sockaddr_storage& peerAddress);

  // Construct a Connection that takes ownership of an existing fd wrapped in BaseFd.
  explicit Connection(BaseFd&& bd) noexcept;

  [[nodiscard]] NativeHandle fd() const noexcept { return _baseFd.fd(); }

  explicit operator bool() const noexcept { return static_cast<bool>(_baseFd); }

  void close() noexcept { _baseFd.close(); }

  bool operator==(const Connection&) const noexcept = default;

#ifdef AERONET_WINDOWS
  // This operator is used in the connections map in SingleHttpServer. This allows usage of transparent look-ups
  // from Fd received from the event loop.
  operator NativeHandle() const noexcept { return _baseFd.fd(); }
#endif

  using trivially_relocatable = amc::is_trivially_relocatable<BaseFd>::type;

 private:
  BaseFd _baseFd;
};

}  // namespace aeronet