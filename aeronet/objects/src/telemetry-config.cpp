#include "aeronet/telemetry-config.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/log.hpp"

namespace aeronet {

void TelemetryConfig::validate() {
  if (sampleRate < 0.0 || sampleRate > 1.0) {
    log::critical("Invalid sample rate {}, must be between 0 and 1", sampleRate);
    throw std::invalid_argument("Invalid sample rate");
  }
  if (dogStatsDEnabled) {
    if (dogstatsdSocketPath().empty()) {
      if (const char* env = std::getenv("DD_DOGSTATSD_SOCKET")) {
        withDogStatsdSocketPath(env);
      }
      if (const char* env = std::getenv("DD_DOGSTATSD_SOCKET_PATH")) {
        withDogStatsdSocketPath(env);
      }
    }
    if (dogstatsdSocketPath().empty()) {
      throw std::invalid_argument("DogStatsD metrics enabled but no socket path configured");
    }
  }

  auto svcName = serviceName();
  if (!svcName.empty()) {
    static constexpr std::string_view kServiceTagPrefix = "service:";
    RawChars serviceTag(kServiceTagPrefix.size() + svcName.size());
    serviceTag.unchecked_append(kServiceTagPrefix);
    serviceTag.unchecked_append(svcName);
    auto tags = dogstatsdTagsRange();
    if (std::ranges::find(tags, serviceTag) == tags.end()) {
      _dogstatsdTags.append(serviceTag);
    }
  }
}

TelemetryConfig& TelemetryConfig::addHttpHeader(std::string_view name, std::string_view value) {
  RawChars header(name.size() + http::HeaderSep.size() + value.size());
  header.unchecked_append(name);
  header.unchecked_append(http::HeaderSep);
  header.unchecked_append(value);
  _httpHeaders.append(header);
  return *this;
}

}  // namespace aeronet