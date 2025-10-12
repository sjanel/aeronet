#include "connection-state.hpp"

#include <sys/types.h>

#include <cstddef>
#include <string_view>

namespace aeronet {

ssize_t ConnectionState::transportRead(std::size_t chunkSize, TransportWant& want) {
  std::size_t oldSize = buffer.size();

  buffer.ensureAvailableCapacity(chunkSize);
  char* writePtr = buffer.data() + oldSize;
  ssize_t bytesRead = transport->read(writePtr, chunkSize, want);
  if (bytesRead > 0) {
    buffer.addSize(static_cast<std::size_t>(bytesRead));
  }
  return bytesRead;
}

ssize_t ConnectionState::transportWrite(std::string_view data, TransportWant& want) {
  const auto res = transport->write(data, want);
  if (!tlsEstablished && !transport->handshakePending()) {
    tlsEstablished = true;
  }
  return res;
}

}  // namespace aeronet