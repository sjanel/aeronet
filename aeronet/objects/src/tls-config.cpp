#include "aeronet/tls-config.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/secure-zero.hpp"
#include "aeronet/tolower-str.hpp"

namespace aeronet {

namespace {
auto NormalizeHostname(std::string_view host) {
  RawChars normalized(host.size());
  tolower_n(host.data(), host.size(), normalized.data());
  normalized.setSize(host.size());
  return normalized;
}

}  // namespace

void TLSConfig::scrubSensitiveData() noexcept {
  _tlsStrings.secureClearPart(kKeyPem);
  clearTlsSessionTicketKeys();
}

void TLSConfig::validate() {
  if (!enabled) {
    return;
  }

  switch (ktlsMode) {
    case KtlsMode::Disabled:
      [[fallthrough]];
    case KtlsMode::Opportunistic:
      [[fallthrough]];
    case KtlsMode::Enabled:
      [[fallthrough]];
    case KtlsMode::Required:
      break;
    default:
      throw std::invalid_argument("Invalid kTLS mode");
  }

  // If TLS config is present we require a cert and a key supplied (either file or in-memory PEM)
  if (certFile().empty() && certPem().empty()) {
    throw std::invalid_argument("TLS configured: certificate missing");
  }
  if (keyFile().empty() && keyPem().empty()) {
    throw std::invalid_argument("TLS configured: private key missing");
  }

  // requireClientCert implies requestClientCert: enforcing mTLS is meaningless unless the server
  // actually asks for a certificate during the handshake. Normalize here so every config path
  // (builders, direct field assignment, JSON/glaze deserialization) converges to a consistent state
  // before the TLS context is built. Without this, requireClientCert=true combined with
  // requestClientCert=false would silently disable client-certificate verification and let
  // certificate-less clients through.
  if (requireClientCert) {
    requestClientCert = true;
  }

  if (requireClientCert && trustedClientCertsPem().empty()) {
    // Policy: require at least one trusted client cert when enforcing mTLS
    throw std::invalid_argument("requireClientCert=true but no trustedClientCertsPem configured");
  }

  // Validate min/max version allowed tokens (if set)
  if (minVersion != Version{} && minVersion != TLS_1_2 && minVersion != TLS_1_3) {
    char buf[Version::kStrLen];
    minVersion.writeFull(buf);
    log::critical("Unsupported tls minVersion '{}', allowed: TLS1.2, TLS1.3", std::string_view(buf, sizeof(buf)));
    throw std::invalid_argument("Unsupported tls minVersion");
  }
  if (maxVersion != Version{} && maxVersion != TLS_1_2 && maxVersion != TLS_1_3) {
    char buf[Version::kStrLen];
    maxVersion.writeFull(buf);
    log::critical("Unsupported tls maxVersion '{}', allowed: TLS1.2, TLS1.3", std::string_view(buf, sizeof(buf)));
    throw std::invalid_argument("Unsupported tls maxVersion");
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
  if ((!crlFile().empty() || revocationCallback != nullptr) && !requestClientCert && !requireClientCert) {
    throw std::invalid_argument("TLS client-certificate revocation checks require client certificate verification");
  }
  if (crlCheckAll && crlFile().empty()) {
    throw std::invalid_argument("crlCheckAll is true but no CRL file is configured");
  }
#ifdef NDEBUG
  if (!keyLogFile().empty()) {
    throw std::invalid_argument("TLS key logging is available only in debug builds");
  }
#endif
}

TLSConfig& TLSConfig::withTlsSessionTicketKey(SessionTicketKey keyMaterial) {
  struct KeyMaterialGuard {
    explicit KeyMaterialGuard(SessionTicketKey& key) : key(key) {}

    KeyMaterialGuard(const KeyMaterialGuard&) = delete;
    KeyMaterialGuard(KeyMaterialGuard&&) = delete;
    KeyMaterialGuard& operator=(const KeyMaterialGuard&) = delete;
    KeyMaterialGuard& operator=(KeyMaterialGuard&&) = delete;

    ~KeyMaterialGuard() { SecureZero(key.data(), key.size()); }

    SessionTicketKey& key;
  } guard(keyMaterial);

  // Avoid leaving an abandoned key copy behind when the vector grows: relocate through a pre-reserved replacement,
  // scrub the old inline elements, then publish the replacement.
  if (_staticTicketKeys.size() == _staticTicketKeys.capacity()) {
    decltype(_staticTicketKeys) replacement;
    replacement.reserve((_staticTicketKeys.size() * 2U) + 1U);
    for (const auto& key : _staticTicketKeys) {
      replacement.push_back(key);
    }
    clearTlsSessionTicketKeys();
    _staticTicketKeys.swap(replacement);
  }
  _staticTicketKeys.push_back(keyMaterial);

  sessionTickets.enabled = true;
  return *this;
}

TLSConfig& TLSConfig::clearTlsSessionTicketKeys() noexcept {
  if (!_staticTicketKeys.empty()) {
    SecureZero(_staticTicketKeys[0].data(), _staticTicketKeys.size() * sizeof(SessionTicketKey));
    _staticTicketKeys.clear();
  }
  return *this;
}

TLSConfig& TLSConfig::withTlsMinVersion(std::string_view ver) {
  minVersion = Version(ver);
  if (minVersion == Version{}) {
    throw std::invalid_argument("Invalid TLS minVersion string");
  }
  return *this;
}

TLSConfig& TLSConfig::withTlsMaxVersion(std::string_view ver) {
  maxVersion = Version(ver);
  if (maxVersion == Version{}) {
    throw std::invalid_argument("Invalid TLS maxVersion string");
  }
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
  const bool isWildcard = ValidateSniCertificateParameters(hostname, certPath, keyPath);
  SniCertificate& entry = _sniCertificates.emplace_back();
  entry.setPattern(NormalizeHostname(hostname));
  entry.isWildcard = isWildcard;
  entry.setCertFile(certPath);
  entry.setKeyFile(keyPath);
  return *this;
}

TLSConfig& TLSConfig::withTlsSniCertificateMemory(std::string_view hostname, std::string_view certPem,
                                                  std::string_view keyPem) {
  const bool isWildcard = ValidateSniCertificateParameters(hostname, certPem, keyPem);
  SniCertificate& entry = _sniCertificates.emplace_back();
  entry.setPattern(NormalizeHostname(hostname));
  entry.isWildcard = isWildcard;
  entry.setCertPem(certPem);
  entry.setKeyPem(keyPem);
  return *this;
}

TLSConfig& TLSConfig::withTlsSniOcspStapleFile(std::string_view hostname, std::string_view derFile) {
  if (hostname.empty() || derFile.empty()) {
    throw std::invalid_argument("SNI hostname and OCSP response file must be non-empty");
  }
  auto normalized = NormalizeHostname(hostname);
  const auto it = std::ranges::find_if(
      _sniCertificates, [&](const SniCertificate& certificate) { return certificate.pattern() == normalized; });
  if (it == _sniCertificates.end()) {
    throw std::invalid_argument("SNI OCSP response has no matching certificate mapping");
  }
  it->setOcspResponseFile(derFile);
  return *this;
}

TLSConfig& TLSConfig::clearTlsSniCertificates() {
  _sniCertificates.clear();
  return *this;
}

}  // namespace aeronet
