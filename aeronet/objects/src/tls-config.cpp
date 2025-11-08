#include "aeronet/tls-config.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "log.hpp"

namespace aeronet {

void TLSConfig::validate() const {
  if (!enabled) {
    return;
  }

  // If TLS config is present we require a cert and a key supplied (either file or in-memory PEM)
  const bool hasCert = !certFile().empty() || !certPem().empty();
  const bool hasKey = !keyFile().empty() || !keyPem().empty();

  if (!hasCert && !hasKey) {
    throw std::invalid_argument("TLS configured but no certificate/key provided");
  }
  if (hasCert && !hasKey) {
    throw std::invalid_argument("TLS configured: certificate present but private key missing");
  }
  if (hasKey && !hasCert) {
    throw std::invalid_argument("TLS configured: private key present but certificate missing");
  }

  if (requireClientCert && trustedClientCertsPem().empty()) {
    // Policy: require at least one trusted client cert when enforcing mTLS
    throw std::invalid_argument("requireClientCert=true but no trustedClientCertsPem configured");
  }

  // Validate min/max version allowed tokens (if set)
  if (minVersion != Version{}) {
    if (minVersion != TLS_1_2 && minVersion != TLS_1_3) {
      log::critical("Unsupported tls minVersion '{}', allowed: TLS1.2, TLS1.3", std::string_view(minVersion.str()));
      throw std::invalid_argument("Unsupported tls minVersion");
    }
  }
  if (maxVersion != Version{}) {
    if (maxVersion != TLS_1_2 && maxVersion != TLS_1_3) {
      log::critical("Unsupported tls maxVersion '{}', allowed: TLS1.2, TLS1.3", std::string_view(maxVersion.str()));
      throw std::invalid_argument("Unsupported tls maxVersion");
    }
  }

  auto alpnProtocols = this->alpnProtocols();
  if (alpnMustMatch && alpnProtocols.empty()) {
    throw std::invalid_argument("alpnMustMatch is true but alpnProtocols is empty");
  }

  if (std::ranges::any_of(alpnProtocols, [](std::string_view proto) { return proto.empty(); })) {
    throw std::invalid_argument("ALPN protocol entries must be non-empty");
  }
  if (std::ranges::any_of(alpnProtocols,
                          [](std::string_view proto) { return std::cmp_less(kMaxAlpnProtocolLength, proto.size()); })) {
    throw std::invalid_argument("ALPN protocol entry exceeds maximum length");
  }
}

}  // namespace aeronet
