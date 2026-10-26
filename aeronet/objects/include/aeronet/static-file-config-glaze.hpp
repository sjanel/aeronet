#pragma once

#include <glaze/glaze.hpp>
#include <string>
#include <string_view>

#include "aeronet/glaze-internal-members.hpp"
#include "aeronet/static-file-config.hpp"

// StaticFileConfig is reflected by glaze: only its internal members (see glaze-internal-members.hpp) need an entry.
template <>
struct glz::meta<aeronet::StaticFileConfig> {
  using T = aeronet::StaticFileConfig;

  // The callbacks are code, not configuration.
  static constexpr bool skip(std::string_view key, const meta_context& /*context*/) noexcept {
    return aeronet::glz_detail::IsInternalMember(key) || key == "contentTypeResolver" ||
           key == "directoryIndexRenderer";
  }

  static constexpr auto modify = object(
      "defaultIndex", custom<[](T& self, const std::string& str) { self.withDefaultIndex(str); }, &T::defaultIndex>,
      "defaultContentType",
      custom<[](T& self, const std::string& str) { self.withDefaultContentType(str); }, &T::defaultContentType>,
      "directoryListingCss",
      custom<[](T& self, const std::string& str) { self.withDirectoryListingCss(str); }, &T::directoryListingCss>);

  static_assert(aeronet::glz_detail::NbObjectEntries<decltype(modify)> == decltype(T::_staticFileStrings)::size(),
                "each packed string of StaticFileConfig needs an entry in modify");
};
