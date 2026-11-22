#include "aeronet/internal/web-session.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/city-hash.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/raw-bytes.hpp"
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
  _chars.reserve(_keySize + value.size());
  _chars.setSize(_keySize);
  _chars.unchecked_append(value);
}

ScriptCache::Script::Script(std::string_view url, std::string_view code) noexcept
    : url(url), code(code), hash(CityHash{}(code)) {}

ScriptCache::Entry* ScriptCache::entryOf(const Script& script) noexcept {
  const auto entryIt = std::ranges::find_if(_entries, [&script](const Entry& entry) {
    return entry.size == script.code.size() && entry.hash == script.hash && std::string_view(entry.url) == script.url;
  });
  return entryIt == _entries.end() ? nullptr : &*entryIt;
}

std::span<const std::byte> ScriptCache::find(const Script& script) noexcept {
  Entry* pEntry = entryOf(script);
  if (pEntry == nullptr) {
    return {};
  }
  pEntry->lastUse = ++_sequence;
  return {pEntry->bytecode.data(), pEntry->bytecode.size()};
}

void ScriptCache::store(const Script& script, std::span<const std::byte> bytecode) {
  if (bytecode.size() > _maxBytes) {
    return;
  }

  Entry* pEntry = entryOf(script);
  if (pEntry == nullptr) {
    pEntry = &_entries.emplace_back(RawChars32(script.url), RawBytes(), script.hash, 0, script.code.size());
  }

  // Counted once copied: a failed copy leaves the entry as it was.
  const std::size_t previousSize = pEntry->bytecode.size();
  pEntry->bytecode.assign(bytecode.data(), bytecode.size());
  _bytes = _bytes - previousSize + bytecode.size();
  pEntry->lastUse = ++_sequence;

  // The new entry, the most recently used one, fits alone: it stays.
  while (_bytes > _maxBytes) {
    const auto oldestIt = std::ranges::min_element(_entries, {}, &Entry::lastUse);
    _bytes -= oldestIt->bytecode.size();
    _entries.erase(oldestIt);
  }
}

StorageAreas::Area& StorageAreas::area(std::string_view origin) {
  const auto areaIt =
      std::ranges::find_if(_areas, [origin](const Area& area) { return std::string_view(area.origin) == origin; });
  if (areaIt != _areas.end()) {
    return *areaIt;
  }

  return _areas.emplace_back(RawChars32(origin));
}

WebSession::WebSession(WebClientConfig cfg)
    : config(WithCookies(std::move(cfg))), http(config.http), scripts(config.maxScriptCacheBytes) {}

}  // namespace aeronet::internal
