#include "aeronet/tls-config.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/log.hpp"
#include "aeronet/major-minor-version.hpp"
#include "aeronet/toupperlower.hpp"

namespace aeronet {

namespace {
std::string NormalizeHostname(std::string_view host) {
  std::string normalized(host);
  for (char& ch : normalized) {
    ch = tolower(ch);
  }
  return normalized;
}

}  // namespace

void TLSConfig::validate() const {
  if (!enabled) {
    return;
  }

#ifndef AERONET_ENABLE_KTLS
  if (ktlsMode != KtlsMode::Disabled) {
    throw std::invalid_argument("KTLS requested but not enabled at build time");
  }
#endif

  // If TLS config is present we require a cert and a key supplied (either file or in-memory PEM)
  const bool hasCert = !certFile().empty() || !certPem().empty();
  const bool hasKey = !keyFile().empty() || !keyPem().empty();
  if (!hasCert) {
    throw std::invalid_argument("TLS configured: certificate missing");
  }
  if (!hasKey) {
    throw std::invalid_argument("TLS configured: private key missing");
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

  if (sessionTickets.maxKeys == 0) {
    throw std::invalid_argument("Session ticket maxKeys must be greater than zero");
  }
  if (!sessionTickets.enabled && !_staticTicketKeys.empty()) {
    throw std::invalid_argument("Session ticket keys configured but tickets disabled");
  }
  if (handshakeRateLimitPerSecond == 0 && handshakeRateLimitBurst != 0) {
    throw std::invalid_argument("TLS handshake rate limit burst set but rate is zero");
  }
}

TLSConfig& TLSConfig::withTlsMinVersion(std::string_view ver) {
  ParseVersion(ver.data(), ver.data() + ver.size(), minVersion);
  return *this;
}

TLSConfig& TLSConfig::withTlsMaxVersion(std::string_view ver) {
  ParseVersion(ver.data(), ver.data() + ver.size(), maxVersion);
  return *this;
}

namespace {
bool ValidateSniCertificateParameters(std::string_view pattern, std::string_view cert, std::string_view key) {
  if (pattern.empty()) {
    throw std::invalid_argument("SNI certificate pattern must be non-empty");
  }
  if (cert.empty() || key.empty()) {
    throw std::invalid_argument("SNI certificate and key must be non-empty");
  }
  bool isWildcard = pattern.starts_with("*.");
  if (isWildcard && pattern.size() == 2) {
    throw std::invalid_argument("Wildcard SNI certificate patterns must start with '*.'");
  }
  return isWildcard;
}
}  // namespace

TLSConfig& TLSConfig::withTlsSniCertificateFiles(std::string_view hostname, std::string_view certPath,
                                                 std::string_view keyPath) {
  bool isWildcard = ValidateSniCertificateParameters(hostname, certPath, keyPath);
  SniCertificate& entry = _sniCertificates.emplace_back();
  entry.setPattern(NormalizeHostname(hostname));
  entry.isWildcard = isWildcard;
  entry.setCertFile(certPath);
  entry.setKeyFile(keyPath);
  return *this;
}

TLSConfig& TLSConfig::withTlsSniCertificateMemory(std::string_view hostname, std::string_view certPem,
                                                  std::string_view keyPem) {
  bool isWildcard = ValidateSniCertificateParameters(hostname, certPem, keyPem);
  SniCertificate& entry = _sniCertificates.emplace_back();
  entry.setPattern(NormalizeHostname(hostname));
  entry.isWildcard = isWildcard;
  entry.setCertPem(certPem);
  entry.setKeyPem(keyPem);
  return *this;
}

TLSConfig& TLSConfig::clearTlsSniCertificates() {
  _sniCertificates.clear();
  return *this;
}

}  // namespace aeronet
