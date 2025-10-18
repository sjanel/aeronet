#include "connection-state.hpp"

#include <chrono>
#include <cstddef>
#include <string_view>

#include "aeronet/http-response-data.hpp"
#include "transport.hpp"

namespace aeronet {

std::size_t ConnectionState::transportRead(std::size_t chunkSize, TransportHint& want) {
  inBuffer.ensureAvailableCapacity(chunkSize);

  const std::size_t bytesRead = transport->read(inBuffer.data() + inBuffer.size(), chunkSize, want);
  inBuffer.addSize(bytesRead);
  if (headerStart.time_since_epoch().count() == 0) {
    headerStart = std::chrono::steady_clock::now();
  }
  return bytesRead;
}

std::size_t ConnectionState::transportWrite(std::string_view data, TransportHint& want) {
  const auto res = transport->write(data, want);
  if (!tlsEstablished && transport->handshakeDone()) {
    tlsEstablished = true;
  }
  return res;
}

std::size_t ConnectionState::transportWrite(const HttpResponseData& httpResponseData, TransportHint& want) {
  const auto res = transport->write(httpResponseData, want);
  if (!tlsEstablished && transport->handshakeDone()) {
    tlsEstablished = true;
  }
  return res;
}

}  // namespace aeronet