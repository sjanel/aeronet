#pragma once

#include <glaze/glaze.hpp>
#include <string>

#include "aeronet/builtin-probes-config.hpp"
#include "aeronet/glaze-chrono-durations-adapters.hpp"  // IWYU pragma: export
#include "aeronet/glaze-internal-members.hpp"

template <>
struct glz::meta<aeronet::BuiltinProbesConfig::ContentType> {
  using enum aeronet::BuiltinProbesConfig::ContentType;
  static constexpr auto value = enumerate("text/plain; charset=utf-8", TextPlainUtf8);
};

// BuiltinProbesConfig is reflected by glaze: only its internal members (see glaze-internal-members.hpp) need an entry.
template <>
struct glz::meta<aeronet::BuiltinProbesConfig> : aeronet::glz_detail::SkipInternalMembers {
  using T = aeronet::BuiltinProbesConfig;
  static constexpr auto modify = object(
      "livenessPath", custom<[](T& self, const std::string& path) { self.withLivenessPath(path); }, &T::livenessPath>,
      "readinessPath",
      custom<[](T& self, const std::string& path) { self.withReadinessPath(path); }, &T::readinessPath>, "startupPath",
      custom<[](T& self, const std::string& path) { self.withStartupPath(path); }, &T::startupPath>);

  static_assert(aeronet::glz_detail::NbObjectEntries<decltype(modify)> == decltype(T::_paths)::size(),
                "each probe path needs an entry in modify");
};
