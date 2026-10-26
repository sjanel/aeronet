#pragma once

#include <glaze/glaze.hpp>
#include <string>
#include <string_view>

#include "aeronet/glaze-chrono-durations-adapters.hpp"  // IWYU pragma: export
#include "aeronet/glaze-internal-members.hpp"
#include "aeronet/major-minor-version-glaze.hpp"  // IWYU pragma: export
#include "aeronet/tls-config.hpp"
#include "aeronet/vector.hpp"

template <>
struct glz::meta<aeronet::TLSConfig::KtlsMode> {
  using enum aeronet::TLSConfig::KtlsMode;
  static constexpr auto value =
      enumerate("disabled", Disabled, "opportunistic", Opportunistic, "enabled", Enabled, "required", Required);
};

template <>
struct glz::meta<aeronet::TLSConfig::CipherPolicy> {
  using enum aeronet::TLSConfig::CipherPolicy;
  static constexpr auto value =
      enumerate("default", Default, "modern", Modern, "compatibility", Compatibility, "legacy", Legacy);
};

template <>
struct glz::meta<aeronet::TLSConfig::SniCertificate> {
  using T = aeronet::TLSConfig::SniCertificate;
  static constexpr auto value = object(
      "pattern",
      custom<[](T& self, const std::string& sv) { self.setPattern(sv); }, [](const T& self) { return self.pattern(); }>,
      "certFile",
      custom<[](T& self, const std::string& sv) { self.setCertFile(sv); },
             [](const T& self) { return self.certFile(); }>,
      "keyFile",
      custom<[](T& self, const std::string& sv) { self.setKeyFile(sv); }, [](const T& self) { return self.keyFile(); }>,
      "certPem",
      custom<[](T& self, const std::string& sv) { self.setCertPem(sv); }, [](const T& self) { return self.certPem(); }>,
      "keyPem",
      custom<[](T& self, const std::string& sv) { self.setKeyPem(sv); }, [](const T& self) { return self.keyPem(); }>,
      "ocspResponseFile",
      custom<[](T& self, const std::string& sv) { self.setOcspResponseFile(sv); },
             [](const T& self) { return self.ocspResponseFile(); }>,
      "isWildcard", &T::isWildcard);
};

// TLSConfig is reflected by glaze: only its internal members (see glaze-internal-members.hpp) need an entry.
template <>
struct glz::meta<aeronet::TLSConfig> {
  using T = aeronet::TLSConfig;

  // The revocation callback and its context are code, not configuration.
  static constexpr bool skip(std::string_view key, const meta_context& /*context*/) noexcept {
    return aeronet::glz_detail::IsInternalMember(key) || key == "revocationCallback" || key == "revocationUserContext";
  }

  static constexpr auto modify = object(
      "certFile", custom<[](T& self, const std::string& str) { self.withCertFile(str); }, &T::certFile>, "keyFile",
      custom<[](T& self, const std::string& str) { self.withKeyFile(str); }, &T::keyFile>, "certPem",
      custom<[](T& self, const std::string& str) { self.withCertPem(str); }, &T::certPem>, "keyPem",
      custom<[](T& self, const std::string& str) { self.withKeyPem(str); }, &T::keyPem>, "cipherList",
      custom<[](T& self, const std::string& str) { self.withCipherList(str); }, &T::cipherList>, "ocspResponseFile",
      custom<[](T& self, const std::string& str) { self.withTlsOcspStapleFile(str); }, &T::ocspResponseFile>, "crlFile",
      custom<[](T& self, const std::string& str) { self.withTlsCrlFile(str, self.crlCheckAll); }, &T::crlFile>,
      "keyLogFile", custom<[](T& self, const std::string& str) { self.withTlsKeyLogFile(str); }, &T::keyLogFile>,
      "alpnProtocols", &T::_alpnProtocols, "trustedClientCertsPem", &T::_trustedClientCertsPem, "sniCertificates",
      custom<[](T& self, const ::aeronet::vector<aeronet::TLSConfig::SniCertificate>& certs) {
        self.clearTlsSniCertificates();
        for (const auto& cert : certs) {
          if (cert.hasFiles()) {
            self.withTlsSniCertificateFiles(cert.pattern(), cert.certFile(), cert.keyFile());
          } else if (cert.hasPem()) {
            self.withTlsSniCertificateMemory(cert.pattern(), cert.certPem(), cert.keyPem());
          }
          if (!cert.ocspResponseFile().empty()) {
            self.withTlsSniOcspStapleFile(cert.pattern(), cert.ocspResponseFile());
          }
        }
      },
             &T::sniCertificates>);

  // One entry per packed string, plus the alpnProtocols, trustedClientCertsPem and sniCertificates lists.
  static_assert(aeronet::glz_detail::NbObjectEntries<decltype(modify)> == decltype(T::_tlsStrings)::size() + 3U,
                "each internal member of TLSConfig needs an entry in modify (or to be explicitly skipped)");
};
