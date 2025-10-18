#include "aeronet/tls-config.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include "invalid_argument_exception.hpp"

namespace aeronet {

void TLSConfig::validate() const {
  if (!enabled) {
    return;
  }

  // If TLS config is present we require a cert and a key supplied (either file or in-memory PEM)
  const bool hasCert = !certFile.empty() || !certPem.empty();
  const bool hasKey = !keyFile.empty() || !keyPem.empty();

  if (!hasCert && !hasKey) {
    throw invalid_argument("TLS configured but no certificate/key provided");
  }
  if (hasCert && !hasKey) {
    throw invalid_argument("TLS configured: certificate present but private key missing");
  }
  if (hasKey && !hasCert) {
    throw invalid_argument("TLS configured: private key present but certificate missing");
  }

  if (requireClientCert && trustedClientCertsPem.empty()) {
    // Policy: require at least one trusted client cert when enforcing mTLS
    throw invalid_argument("requireClientCert=true but no trustedClientCertsPem configured");
  }

  // Validate min/max version allowed tokens (if set)
  if (!minVersion.empty()) {
    if (minVersion != "TLS1.2" && minVersion != "TLS1.3") {
      throw invalid_argument("Unsupported tls minVersion '{}', allowed: TLS1.2, TLS1.3", minVersion);
    }
  }
  if (!maxVersion.empty()) {
    if (maxVersion != "TLS1.2" && maxVersion != "TLS1.3") {
      throw invalid_argument("Unsupported tls maxVersion '{}', allowed: TLS1.2, TLS1.3", maxVersion);
    }
  }

  if (alpnMustMatch && alpnProtocols.empty()) {
    throw invalid_argument("alpnMustMatch is true but alpnProtocols is empty");
  }

  if (std::ranges::any_of(alpnProtocols, [](std::string_view proto) { return proto.empty(); })) {
    throw invalid_argument("ALPN protocol entries must be non-empty");
  }
  if (std::ranges::any_of(alpnProtocols,
                          [](std::string_view proto) { return std::cmp_less(kMaxAlpnProtocolLength, proto.size()); })) {
    throw invalid_argument("ALPN protocol entry exceeds maximum length {}", kMaxAlpnProtocolLength);
  }
}

}  // namespace aeronet
