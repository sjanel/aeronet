#include "connection-state.hpp"

#include <cstddef>
#include <string_view>

#include "aeronet/http-response-data.hpp"
#include "transport.hpp"

namespace aeronet {

std::size_t ConnectionState::transportRead(std::size_t chunkSize, Transport& want) {
  buffer.ensureAvailableCapacity(chunkSize);

  const std::size_t bytesRead = transport->read(buffer.data() + buffer.size(), chunkSize, want);
  buffer.addSize(bytesRead);
  return bytesRead;
}

std::size_t ConnectionState::transportWrite(std::string_view data, Transport& want) {
  const auto res = transport->write(data, want);
  if (!tlsEstablished && transport->handshakeDone()) {
    tlsEstablished = true;
  }
  return res;
}

std::size_t ConnectionState::transportWrite(const HttpResponseData& httpResponseData, Transport& want) {
  const auto res = transport->write(httpResponseData, want);
  if (!tlsEstablished && transport->handshakeDone()) {
    tlsEstablished = true;
  }
  return res;
}

}  // namespace aeronet