#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "aeronet/platform.hpp"

namespace aeronet {

struct TlsHandshakeEvent {
  enum class Result : uint8_t { Succeeded, Failed, Rejected };

  NativeHandle fd{kInvalidHandle};

  Result result{Result::Succeeded};

  bool resumed{false};
  bool clientCertPresent{false};

  uint64_t durationNs{0};

  // Stable reason identifier for Failed / Rejected (empty for success).
  // Note: views are only guaranteed to be valid during the callback.
  std::string_view reason;
  std::string_view selectedAlpn;
  std::string_view negotiatedCipher;
  std::string_view negotiatedVersion;
  std::string_view peerSubject;
};

using TlsHandshakeCallback = std::function<void(const TlsHandshakeEvent&)>;

}  // namespace aeronet