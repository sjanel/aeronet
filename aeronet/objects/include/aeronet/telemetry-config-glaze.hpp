#pragma once

#include <glaze/glaze.hpp>
#include <map>
#include <string>

#include "aeronet/glaze-chrono-durations-adapters.hpp"  // IWYU pragma: export
#include "aeronet/glaze-internal-members.hpp"
#include "aeronet/glaze-vector-sv-adapter.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/vector.hpp"

// TelemetryConfig is reflected by glaze: only its internal members (see glaze-internal-members.hpp) need an entry.
template <>
struct glz::meta<aeronet::TelemetryConfig> : aeronet::glz_detail::SkipInternalMembers {
  using T = aeronet::TelemetryConfig;
  static constexpr auto modify = object(
      "endpoint", custom<[](T& self, const std::string& str) { self.withEndpoint(str); }, &T::endpoint>, "serviceName",
      custom<[](T& self, const std::string& str) { self.withServiceName(str); }, &T::serviceName>,
      "dogstatsdSocketPath",
      custom<[](T& self, const std::string& str) { self.withDogStatsdSocketPath(str); }, &T::dogstatsdSocketPath>,
      "dogstatsdNamespace",
      custom<[](T& self, const std::string& str) { self.withDogStatsdNamespace(str); }, &T::dogstatsdNamespace>,
      // Tags as array of "key:value" strings
      "dogstatsdTags", &T::_dogstatsdTags,
      // HTTP headers as array of "name:value" strings
      "httpHeaders",
      custom<[](T& self, const ::aeronet::vector<std::string>& headers) {
        for (const auto& hdr : headers) {
          if (auto sep = hdr.find(':'); sep != std::string::npos) {
            self.addHttpHeader(std::string_view(hdr).substr(0, sep), std::string_view(hdr).substr(sep + 1));
          }
        }
      },
             aeronet::glz_detail::ToStringViewVector<&T::httpHeadersRange>>,
      // Histogram buckets as JSON object {"metric_name": [1.0, 5.0, 10.0]}
      "histogramBuckets",
      custom<[](T& self, std::map<std::string, ::aeronet::vector<double>> buckets) {
        for (auto& [name, boundaries] : buckets) {
          self.addHistogramBuckets(name, boundaries);
        }
      },
             [](const T& self) {
               std::map<std::string, ::aeronet::vector<double>> result;
               for (const auto& [key, boundaries] : self.histogramBuckets()) {
                 result[std::string(std::string_view(key))] =
                     ::aeronet::vector<double>(boundaries.begin(), boundaries.end());
               }
               return result;
             }>);

  // One entry per packed string, plus the dogstatsdTags, httpHeaders and histogramBuckets lists.
  static_assert(aeronet::glz_detail::NbObjectEntries<decltype(modify)> == decltype(T::_staticStrings)::size() + 3U,
                "each internal member of TelemetryConfig needs an entry in modify");
};
