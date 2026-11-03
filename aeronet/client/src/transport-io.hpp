#pragma once

#include <cstddef>

#include "aeronet/event.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"

namespace aeronet::internal {

// The events to wait for before retrying a transport operation that returned `want`: readability for ReadReady,
// writability otherwise.
constexpr EventBmp InterestFor(TransportHint want) noexcept {
  return want == TransportHint::ReadReady ? EventIn : EventOut;
}

// Read at most `maxBytes` from `transport` at the end of `buf`, which grows (exponentially) to hold them.
inline TransportResult ReadAppend(Transport& transport, RawChars& buf, std::size_t maxBytes) {
  buf.ensureAvailableCapacityExponential(maxBytes);
  const TransportResult transportRes = transport.read(buf.data() + buf.size(), maxBytes);
  buf.addSize(transportRes.bytesProcessed);
  return transportRes;
}

}  // namespace aeronet::internal
