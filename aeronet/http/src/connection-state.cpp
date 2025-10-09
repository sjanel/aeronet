#include "connection-state.hpp"

#include <sys/types.h>

#include <cstddef>
#include <string_view>

namespace aeronet {

ssize_t ConnectionState::transportRead(std::size_t chunkSize, bool& wantRead, bool& wantWrite) {
  std::size_t oldSize = buffer.size();

  buffer.ensureAvailableCapacity(chunkSize);
  char* writePtr = buffer.data() + oldSize;
  ssize_t bytesRead = transport->read(writePtr, chunkSize, wantRead, wantWrite);
  if (bytesRead > 0) {
    buffer.addSize(static_cast<std::size_t>(bytesRead));
  }
  return bytesRead;
}

ssize_t ConnectionState::transportWrite(std::string_view data, bool& wantRead, bool& wantWrite) const {
  return transport->write(data, wantRead, wantWrite);
}

}  // namespace aeronet