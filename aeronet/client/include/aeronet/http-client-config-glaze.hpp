#pragma once

#include <glaze/glaze.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "aeronet/client-protocol-glaze.hpp"            // IWYU pragma: export
#include "aeronet/direct-compression-mode-glaze.hpp"    // IWYU pragma: export
#include "aeronet/encoding-glaze.hpp"                   // IWYU pragma: export
#include "aeronet/glaze-chrono-durations-adapters.hpp"  // IWYU pragma: export
#include "aeronet/glaze-internal-members.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/major-minor-version-glaze.hpp"  // IWYU pragma: export
#include "aeronet/tcp-no-delay-mode-glaze.hpp"    // IWYU pragma: export
#include "aeronet/telemetry-config-glaze.hpp"     // IWYU pragma: export
#include "aeronet/vector.hpp"

namespace aeronet::glz_detail {

// Method set from method names (case-sensitive). Throws std::invalid_argument on an unknown name.
inline http::MethodBmp MethodBmpFromNames(const vector<std::string>& names) {
  http::MethodBmp methods = 0;
  for (const auto& name : names) {
    const auto method = http::ParseMethodStr(name);
    if (method == http::kMethodInvalid) {
      throw std::invalid_argument("Unknown HTTP method in cache.methods: '" + name + "'");
    }
    methods |= static_cast<http::MethodBmp>(method);
  }
  return methods;
}

// Names of the methods of a method set.
inline vector<std::string_view> MethodBmpToNames(http::MethodBmp methods) {
  vector<std::string_view> names;
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    if (http::IsMethodIdxSet(methods, methodIdx)) {
      names.push_back(http::MethodIdxToStr(methodIdx));
    }
  }
  return names;
}

}  // namespace aeronet::glz_detail

// The cacheable methods, as an array of method names (e.g. ["GET", "HEAD"]).
template <>
struct glz::meta<aeronet::HttpClientConfig::RequestCache> {
  using T = aeronet::HttpClientConfig::RequestCache;
  static constexpr auto modify =
      object("methods", custom<[](T& self, const ::aeronet::vector<std::string>& names) {
               self.methods = aeronet::glz_detail::MethodBmpFromNames(names);
             },
                               [](const T& self) { return aeronet::glz_detail::MethodBmpToNames(self.methods); }>);
};

// HttpClientConfig is reflected by glaze: only its packed strings (see glaze-internal-members.hpp) need an entry.
template <>
struct glz::meta<aeronet::HttpClientConfig> : aeronet::glz_detail::SkipInternalMembers {
  using T = aeronet::HttpClientConfig;
  static constexpr auto modify = object(
      "defaultAcceptEncoding",
      custom<[](T& self, const std::string& str) { self.withDefaultAcceptEncoding(str); }, &T::defaultAcceptEncoding>,
      "proxyUrl", custom<[](T& self, const std::string& str) { self.withProxy(str); }, &T::proxyUrl>, "proxyCaFile",
      custom<[](T& self, const std::string& str) { self._strings.set(T::kProxyCaFile, str); }, &T::proxyCaFile>
#ifdef AERONET_ENABLE_OPENSSL
      ,
      "tlsCaFile", custom<[](T& self, const std::string& str) { self.withTlsCaFile(str); }, &T::tlsCaFile>, "tlsCaPath",
      custom<[](T& self, const std::string& str) { self.withTlsCaPath(str); }, &T::tlsCaPath>, "tlsCipherList",
      custom<[](T& self, const std::string& str) { self.withTlsCipherList(str); }, &T::tlsCipherList>,
      "tlsClientCertFile",
      custom<[](T& self, const std::string& str) { self._strings.set(T::kClientCertFile, str); },
             &T::tlsClientCertFile>,
      "tlsClientKeyFile",
      custom<[](T& self, const std::string& str) { self._strings.set(T::kClientKeyFile, str); }, &T::tlsClientKeyFile>,
      "tlsClientCertPem",
      custom<[](T& self, const std::string& str) { self._strings.set(T::kClientCertPem, str); }, &T::tlsClientCertPem>,
      "tlsClientKeyPem",
      custom<[](T& self, const std::string& str) { self._strings.set(T::kClientKeyPem, str); }, &T::tlsClientKeyPem>
#endif
  );

  static_assert(aeronet::glz_detail::NbObjectEntries<decltype(modify)> == decltype(T::_strings)::size(),
                "each packed string of HttpClientConfig needs an entry in modify");
};
