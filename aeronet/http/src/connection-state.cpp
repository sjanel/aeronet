#include "connection-state.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <string_view>

namespace aeronet {

ssize_t ConnectionState::transportRead(int fd, std::size_t chunkSize, bool& wantRead, bool& wantWrite) {
  std::size_t oldSize = buffer.size();
  ssize_t bytesRead = 0;
  buffer.resize_and_overwrite(oldSize + chunkSize, [&](char* base, std::size_t /*n*/) {
    char* writePtr = base + oldSize;  // base points to beginning of existing buffer
    if (transport) {
      bytesRead = transport->read(writePtr, chunkSize, wantRead, wantWrite);
    } else {
      wantRead = wantWrite = false;
      bytesRead = ::read(fd, writePtr, chunkSize);
    }
    if (bytesRead > 0) {
      return oldSize + static_cast<std::size_t>(bytesRead);  // grew by bytesRead
    }
    return oldSize;  // retain previous logical size on EOF / EAGAIN / error
  });
  return bytesRead;
}

ssize_t ConnectionState::transportWrite(int fd, std::string_view data, bool& wantRead, bool& wantWrite) const {
  if (transport) {
    return transport->write(data, wantRead, wantWrite);
  }
  wantRead = false;
  wantWrite = false;
  return ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
}

}  // namespace aeronet