#include "aeronet/internal/web-session.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>

#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/web-client-config.hpp"

namespace aeronet::internal {

namespace {

// The pages have a document.cookie: the cookie jar of the HTTP client is enabled.
WebClientConfig WithCookies(WebClientConfig config) {
  if (!config.http.cookies.enabled()) {
    config.http.withCookies();
  }
  return config;
}

}  // namespace

StorageAreas::Item::Item(std::string_view key, std::string_view value)
    : _chars(key.size() + value.size()), _keySize(static_cast<uint32_t>(key.size())) {
  _chars.unchecked_append(key);
  _chars.unchecked_append(value);
}

void StorageAreas::Item::setValue(std::string_view value) {
  _chars.setSize(_keySize);
  _chars.append(value);
}

StorageAreas::Area& StorageAreas::area(std::string_view origin) {
  const auto areaIt =
      std::ranges::find_if(_areas, [origin](const Area& area) { return std::string_view(area.origin) == origin; });
  if (areaIt != _areas.end()) {
    return *areaIt;
  }
  _areas.push_back({RawChars32(origin), {}, 0});
  return _areas.back();
}

WebSession::WebSession(WebClientConfig cfg) : config(WithCookies(std::move(cfg))), http(config.http) {}

}  // namespace aeronet::internal
