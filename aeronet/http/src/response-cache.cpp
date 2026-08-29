#include "aeronet/response-cache.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/header-line-parse.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/tolower-str.hpp"
#include "aeronet/toupperlower.hpp"

namespace aeronet {

namespace {

using Clock = std::chrono::steady_clock;
using Entry = internal::ResponseCacheEntry;
using Prototype = internal::ResponseCachePrototype;
using State = internal::ResponseCacheState;
using VariantIdentity = internal::ResponseCacheVariantIdentity;
using VaryField = internal::ResponseCacheVaryField;
using VariantIndex = vector<Entry*>::size_type;
using VaryIndex = vector<VaryField>::size_type;

constexpr uint64_t kAgeNotSpecified = std::numeric_limits<uint64_t>::max();

enum class RemovalReason : uint8_t { None, Eviction, Expiration };

struct CacheControlSummary {
  bool noStore{};
  bool noCache{};
  bool isPrivate{};
  bool invalidFreshness{};
  uint64_t maxAge{kAgeNotSpecified};
  uint64_t sharedMaxAge{kAgeNotSpecified};
};

bool CaseInsensitiveStartsWith(std::string_view value, std::string_view prefix) noexcept {
  return value.size() >= prefix.size() && CaseInsensitiveEqual(value.substr(0, prefix.size()), prefix);
}

bool CaseInsensitiveLess(std::string_view lhs, std::string_view rhs) noexcept {
  const std::size_t commonSize = std::min(lhs.size(), rhs.size());
  for (std::size_t i = 0; i < commonSize; ++i) {
    const char left = tolower(lhs[i]);
    const char right = tolower(rhs[i]);
    if (left != right) {
      return left < right;
    }
  }
  return lhs.size() < rhs.size();
}

void AppendLengthAndValue(RawChars32& output, std::string_view value) {
  const uint64_t length = value.size();
  output.unchecked_append(reinterpret_cast<const char*>(&length), sizeof(length));
  output.unchecked_append(value);
}

void AppendLowerLengthAndValue(RawChars32& output, std::string_view value) {
  const uint64_t length = value.size();
  output.unchecked_append(reinterpret_cast<const char*>(&length), sizeof(length));
  tolower_n(value.data(), value.size(), output.data() + output.size());
  output.addSize(static_cast<uint32_t>(value.size()));
}

void BuildPrimaryKey(const HttpRequestView& request, RawChars32& key) {
  std::string_view scheme;
  std::string_view authority;
#ifdef AERONET_ENABLE_HTTP2
  if (request.isHttp2()) {
    scheme = request.scheme();
    authority = request.authority();
  }
#endif
  if (scheme.empty()) {
    scheme = request.tlsVersion().empty() ? std::string_view{"http"} : std::string_view{"https"};
  }
  if (authority.empty()) {
    authority = request.headerValueOrEmpty(http::Host);
  }

  uint64_t keySize = 1U + (3U * sizeof(uint64_t)) + scheme.size() + authority.size() + request.path().size();
  for (const auto [queryKey, queryValue] : request.queryParamsRange()) {
    keySize += 1U + (2U * sizeof(uint64_t)) + queryKey.size() + queryValue.size();
  }

  key.clear();
  key.reserve(keySize);
  key.unchecked_push_back(static_cast<char>(http::MethodToIdx(request.method())));

  AppendLowerLengthAndValue(key, scheme);
  AppendLowerLengthAndValue(key, authority);
  AppendLengthAndValue(key, request.path());
  for (const auto [queryKey, queryValue] : request.queryParamsRange()) {
    key.unchecked_push_back('\1');
    AppendLengthAndValue(key, queryKey);
    AppendLengthAndValue(key, queryValue);
  }
}

bool RequestMethodEligible(const ResponseCacheConfig& config, const HttpRequestView& request) noexcept {
  return http::IsMethodSet(config.methods, request.method());
}

bool RequestHasSensitiveOrRangeHeaders(const ResponseCacheConfig& config, const HttpRequestView& request) noexcept {
  return (config.bypassAuthorization && request.hasHeader(http::Authorization)) ||
         (config.bypassCookie && request.hasHeader(http::Cookie)) || request.hasHeader(http::Range) ||
         request.hasHeader(http::IfRange);
}

bool ParseUnsignedSeconds(std::string_view value, uint64_t& seconds) noexcept {
  value = TrimOws(value);
  if (value.size() >= 2U && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2U);
  }
  if (value.empty()) {
    return false;
  }
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
  return error == std::errc{} && end == value.data() + value.size();
}

void AssignFreshness(uint64_t& destination, std::string_view value, CacheControlSummary& summary) noexcept {
  uint64_t parsed{};
  if (!ParseUnsignedSeconds(value, parsed) || (destination != kAgeNotSpecified && destination != parsed)) {
    summary.invalidFreshness = true;
    return;
  }
  destination = parsed;
}

void ParseCacheControlValue(std::string_view value, CacheControlSummary& summary) noexcept {
  while (!value.empty()) {
    const auto comma = value.find(',');
    const std::string_view directive = TrimOws(value.substr(0, comma));
    if (comma == std::string_view::npos) {
      value = {};
    } else {
      value.remove_prefix(comma + 1U);
    }
    if (directive.empty()) {
      continue;
    }
    const auto equals = directive.find('=');
    const std::string_view name = TrimOws(directive.substr(0, equals));
    const std::string_view argument =
        equals == std::string_view::npos ? std::string_view{} : TrimOws(directive.substr(equals + 1U));
    if (CaseInsensitiveEqual(name, "no-store")) {
      summary.noStore = true;
    } else if (CaseInsensitiveEqual(name, "no-cache")) {
      summary.noCache = true;
    } else if (CaseInsensitiveEqual(name, "private")) {
      summary.isPrivate = true;
    } else if (CaseInsensitiveEqual(name, "max-age")) {
      AssignFreshness(summary.maxAge, argument, summary);
    } else if (CaseInsensitiveEqual(name, "s-maxage")) {
      AssignFreshness(summary.sharedMaxAge, argument, summary);
    }
  }
}

template <class Fn>
void ForEachEffectiveHeader(const HttpResponse& response, const ConcatenatedHeaders* globalHeaders,
                            std::string_view wantedName, Fn&& fn) {
  bool foundInResponse = false;
  for (const auto [name, value] : response.headers()) {
    if (CaseInsensitiveEqual(name, wantedName)) {
      foundInResponse = true;
      fn(value);
    }
  }
  if (foundInResponse || globalHeaders == nullptr) {
    return;
  }
  for (const std::string_view line : *globalHeaders) {
    const auto header = http::ParseHeaderLine(line.data(), line.data() + line.size());
    if (CaseInsensitiveEqual(header.name, wantedName)) {
      fn(header.value);
    }
  }
}

CacheControlSummary EffectiveCacheControl(const HttpResponse& response, const ConcatenatedHeaders* globals) noexcept {
  CacheControlSummary summary;
  ForEachEffectiveHeader(response, globals, http::CacheControl,
                         [&summary](std::string_view value) { ParseCacheControlValue(value, summary); });
  return summary;
}

CacheControlSummary RequestCacheControl(const HttpRequestView& request) noexcept {
  CacheControlSummary summary;
  if (const auto value = request.headerValue(http::CacheControl)) {
    ParseCacheControlValue(*value, summary);
  }
  return summary;
}

std::chrono::seconds FreshnessLifetime(const ResponseCacheConfig& config, const CacheControlSummary& summary) noexcept {
  if (summary.noStore || summary.noCache || summary.isPrivate || summary.invalidFreshness) {
    return std::chrono::seconds::zero();
  }
  const uint64_t configuredMaximum = config.maximumAge == std::chrono::seconds::max()
                                         ? std::numeric_limits<uint64_t>::max()
                                         : static_cast<uint64_t>(config.maximumAge.count());
  uint64_t raw;
  if (summary.sharedMaxAge == kAgeNotSpecified) {
    if (summary.maxAge == kAgeNotSpecified) {
      raw = static_cast<uint64_t>(config.defaultMaxAge.count());
    } else {
      raw = summary.maxAge;
    }
  } else {
    raw = summary.sharedMaxAge;
  }

  const uint64_t capped = std::min(raw, configuredMaximum);
  if (capped == 0U) {
    return std::chrono::seconds::zero();
  }
  const uint64_t chronoMax = static_cast<uint64_t>(std::chrono::seconds::max().count());
  return std::chrono::seconds(static_cast<int64_t>(std::min(capped, chronoMax)));
}

bool EffectiveVaryNames(const HttpResponse& response, const ConcatenatedHeaders* globals,
                        vector<std::string_view>& names) {
  names.clear();
  bool valid = true;
  ForEachEffectiveHeader(response, globals, http::Vary, [&](std::string_view value) {
    while (!value.empty() && valid) {
      const auto comma = value.find(',');
      const std::string_view name = TrimOws(value.substr(0, comma));
      if (comma == std::string_view::npos) {
        value = {};
      } else {
        value.remove_prefix(comma + 1U);
      }
      if (name == "*" || name.empty() || !http::IsValidHeaderName(name)) {
        valid = false;
        return;
      }
      names.emplace_back(name);
    }
  });
  std::ranges::sort(names, CaseInsensitiveLess);
  names.erase(std::ranges::unique(names, CaseInsensitiveEqual).begin(), names.end());
  return valid;
}

VariantIdentity CaptureVariantIdentity(const HttpRequestView& request, const vector<std::string_view>& names,
                                       State& state) {
  uint64_t normalizedNamesSize{};
  for (const std::string_view name : names) {
    normalizedNamesSize += name.size();
  }
  state.normalizedVaryNameViewsScratch.clear();
  state.normalizedVaryNamesScratch.clear();
  state.normalizedVaryNamesScratch.reserve(normalizedNamesSize);
  state.normalizedVaryNameViewsScratch.reserve(names.size());
  for (const std::string_view name : names) {
    const char* const nameData = state.normalizedVaryNamesScratch.data() + state.normalizedVaryNamesScratch.size();
    tolower_n(name.data(), name.size(),
              state.normalizedVaryNamesScratch.data() + state.normalizedVaryNamesScratch.size());
    state.normalizedVaryNamesScratch.addSize(static_cast<uint32_t>(name.size()));
    state.normalizedVaryNameViewsScratch.emplace_back(nameData, name.size());
  }

  uint64_t stringsSize = request.path().size();
  for (const std::string_view name : state.normalizedVaryNameViewsScratch) {
    const auto value = request.headerValue(LowerAsciiKey{name});
    stringsSize += name.size() + (value ? value->size() : 0U);
  }

  VariantIdentity identity;
  identity.strings = RawChars32(stringsSize);
  const char* const pathData = identity.strings.data();
  identity.strings.unchecked_append(request.path());
  identity.path = std::string_view{pathData, request.path().size()};
  identity.fields.reserve(state.normalizedVaryNameViewsScratch.size());
  for (const std::string_view name : state.normalizedVaryNameViewsScratch) {
    const auto value = request.headerValue(LowerAsciiKey{name});
    const char* const nameData = identity.strings.data() + identity.strings.size();
    identity.strings.unchecked_append(name);
    const char* const valueData = identity.strings.data() + identity.strings.size();
    if (value) {
      identity.strings.unchecked_append(*value);
    }
    identity.fields.emplace_back(std::string_view{nameData, name.size()},
                                 value ? std::string_view{valueData, value->size()} : std::string_view{});
  }
  return identity;
}

bool VaryMatches(const Entry& entry, const HttpRequestView& request) noexcept {
  return std::ranges::all_of(entry.varyFields, [&request](const VaryField& field) {
    const auto requestValue = request.headerValue(LowerAsciiKey{field.name});
    const bool storedValuePresent = field.value.data() != nullptr;
    return requestValue.has_value() == storedValuePresent && (!requestValue || *requestValue == field.value);
  });
}

bool SameVarySchema(const Entry& entry, const vector<VaryField>& fields) noexcept {
  if (entry.varyFields.size() != fields.size()) {
    return false;
  }
  for (VaryIndex i = 0; i < fields.size(); ++i) {
    if (entry.varyFields[i].name != fields[i].name) {
      return false;
    }
  }
  return true;
}

bool SameVariant(const Entry& entry, const vector<VaryField>& fields) noexcept {
  if (!SameVarySchema(entry, fields)) {
    return false;
  }
  for (VaryIndex i = 0; i < fields.size(); ++i) {
    const bool entryValuePresent = entry.varyFields[i].value.data() != nullptr;
    const bool fieldValuePresent = fields[i].value.data() != nullptr;
    if (entryValuePresent != fieldValuePresent || entry.varyFields[i].value != fields[i].value) {
      return false;
    }
  }
  return true;
}

void Detach(State& state, Entry& entry) noexcept {
  if (entry.newer != nullptr) {
    entry.newer->older = entry.older;
  } else {
    state.newest = entry.older;
  }
  if (entry.older != nullptr) {
    entry.older->newer = entry.newer;
  } else {
    state.oldest = entry.newer;
  }
  entry.newer = nullptr;
  entry.older = nullptr;
}

void AttachNewest(State& state, Entry& entry) noexcept {
  entry.newer = nullptr;
  entry.older = state.newest;
  if (state.newest != nullptr) {
    state.newest->newer = &entry;
  } else {
    state.oldest = &entry;
  }
  state.newest = &entry;
  entry.accessSequence = ++state.accessSequence;
}

void Touch(State& state, Entry& entry) noexcept {
  if (state.newest != &entry) {
    Detach(state, entry);
    AttachNewest(state, entry);
  } else {
    entry.accessSequence = ++state.accessSequence;
  }
}

void RemoveAt(State& state, decltype(State::entries)::iterator mapIt, VariantIndex index,
              RemovalReason reason) noexcept {
  auto& variants = mapIt->second;
  Entry* const entry = variants[index];
  Detach(state, *entry);
  --state.currentEntries;
  state.currentMemoryBytes -= entry->memoryCharge;
  variants.erase(variants.begin() + static_cast<std::ptrdiff_t>(index));
  state.entryPool.destroyAndRelease(entry);
  if (variants.empty()) {
    state.entries.erase(mapIt);
  }
  if (reason == RemovalReason::Eviction) {
    ++state.evictions;
  } else if (reason == RemovalReason::Expiration) {
    ++state.expirations;
  }
}

bool RemoveEntry(State& state, Entry* target, RemovalReason reason) noexcept {
  if (target == nullptr) {
    return false;
  }
  auto mapIt = state.entries.find(target->primaryKey);
  if (mapIt == state.entries.end()) {
    return false;
  }
  for (VariantIndex i = 0; i < mapIt->second.size(); ++i) {
    if (mapIt->second[i] == target) {
      RemoveAt(state, mapIt, i, reason);
      return true;
    }
  }
  return false;
}

std::size_t RemovePrimary(State& state, std::string_view primaryKey) noexcept {
  std::size_t removed{};
  auto mapIt = state.entries.find(primaryKey);
  while (mapIt != state.entries.end()) {
    ++removed;
    RemoveAt(state, mapIt, mapIt->second.size() - 1U, RemovalReason::None);
    mapIt = state.entries.find(primaryKey);
  }
  return removed;
}

Prototype CapturePrototype(const HttpResponse& response) {
  Prototype prototype;

  prototype.status = response.status();
  prototype.hadBody = response.hasBody();
  prototype.sizeOnlyBody = response.hasBody() && !response.hasBodyInMemory() && !response.hasBodyFile();

  const std::string_view reason = response.reason();
  const std::string_view contentTypeHeader = response.headerValueOrEmpty(http::ContentType);
  const std::string_view bodyInMemory = response.bodyInMemory();
  std::size_t totalStringsSz = reason.size() + contentTypeHeader.size() + bodyInMemory.size();
  uint32_t nbHeaders{};
  for (const auto [name, value] : response.headers()) {
    if (name == http::ContentType) {
      break;
    }
    assert(name != http::ContentLength);
    totalStringsSz += http::HeaderSize(name.size(), value.size());
    ++nbHeaders;
  }
  prototype.nbHeaders = nbHeaders;

  prototype.buf = std::make_unique_for_overwrite<char[]>(totalStringsSz);
  char* pData = prototype.buf.get();
  prototype.reason = std::string_view{pData, reason.size()};
  pData = Append(reason, pData);
  prototype.contentType = std::string_view{pData, contentTypeHeader.size()};
  pData = Append(contentTypeHeader, pData);
  prototype.body = std::string_view{pData, bodyInMemory.size()};
  pData = Append(bodyInMemory, pData);
  prototype.headers =
      HeadersView{std::string_view(pData, totalStringsSz - static_cast<std::size_t>(pData - prototype.buf.get()))};
  for (const auto [name, value] : response.headers()) {
    if (name == http::ContentType) {
      break;
    }
    pData = WriteHeaderCRLF(name, value, pData);
  }
  assert(pData == prototype.buf.get() + totalStringsSz);

  prototype.bodySize = response.bodySize();
  prototype.memoryCharge = totalStringsSz + sizeof(Prototype);
  return prototype;
}

bool FirstEffectiveHeader(const HttpResponse& response, const ConcatenatedHeaders* globals, std::string_view name,
                          std::string_view& result) noexcept {
  bool found = false;
  ForEachEffectiveHeader(response, globals, name, [&found, &result](std::string_view value) {
    if (!found) {
      result = value;
      found = true;
    }
  });
  return found;
}

std::string_view StripWeakPrefix(std::string_view tag) noexcept {
  tag = TrimOws(tag);
  if (tag.size() >= 2U && (tag[0] == 'W' || tag[0] == 'w') && tag[1] == '/') {
    tag.remove_prefix(2U);
    tag = TrimOws(tag);
  }
  return tag;
}

bool IsEntityTag(std::string_view tag) noexcept { return tag.size() >= 2U && tag.front() == '"' && tag.back() == '"'; }

bool IfNoneMatchMatches(std::string_view condition, std::string_view currentTag) noexcept {
  currentTag = StripWeakPrefix(currentTag);
  if (!IsEntityTag(currentTag)) {
    return false;
  }
  while (!condition.empty()) {
    bool insideTag = false;
    std::size_t comma = std::string_view::npos;
    for (std::size_t index = 0; index < condition.size(); ++index) {
      if (condition[index] == '"') {
        insideTag = !insideTag;
      } else if (condition[index] == ',' && !insideTag) {
        comma = index;
        break;
      }
    }
    const std::string_view candidate = TrimOws(condition.substr(0, comma));
    if (comma == std::string_view::npos) {
      condition = {};
    } else {
      condition.remove_prefix(comma + 1U);
    }
    if (candidate == "*") {
      return true;
    }
    const std::string_view weakCandidate = StripWeakPrefix(candidate);
    if (IsEntityTag(weakCandidate) && weakCandidate == currentTag) {
      return true;
    }
  }
  return false;
}

bool IsConditionalMetadata(std::string_view name) noexcept {
  return name == std::string_view(http::ETag) || name == std::string_view(http::Vary) ||
         name == std::string_view(http::CacheControl) || name == std::string_view(http::Expires) ||
         name == std::string_view(http::ContentLocation) || name == std::string_view(http::LastModified) ||
         CaseInsensitiveStartsWith(name, "access-control-");
}

void CopyHeaderReplacingGlobals(HttpResponse& destination, const HttpResponse& source, std::string_view name) {
  const LowerAsciiKey key{name};
  while (destination.hasHeader(key)) {
    destination.headerRemoveLine(key);
  }
  for (const auto [sourceName, value] : source.headers()) {
    if (sourceName == name) {
      destination.headerAddLine(key, value);
    }
  }
}

std::size_t EntryCharge(const RawChars32& primaryKey, const VariantIdentity& identity,
                        const Prototype& prototype) noexcept {
  return sizeof(Entry) + primaryKey.size() + identity.strings.capacity() +
         (identity.fields.capacity() * sizeof(VaryField)) + prototype.memoryCharge;
}

void ValidateConfig(const ResponseCacheConfig& config) {
  const http::MethodBmp supportedMethods = http::Method::GET | http::Method::HEAD;
  if (config.maxEntries == 0U) {
    throw std::invalid_argument("response cache maxEntries must be at least 1");
  }
  if (config.maxMemoryBytes == 0U) {
    throw std::invalid_argument("response cache maxMemoryBytes must be at least 1");
  }
  if (config.maxEntryBytes == 0U || config.maxEntryBytes > config.maxMemoryBytes) {
    throw std::invalid_argument("response cache maxEntryBytes must be in [1, maxMemoryBytes]");
  }
  if (config.maxVariantsPerTarget == 0U) {
    throw std::invalid_argument("response cache maxVariantsPerTarget must be at least 1");
  }
  if (config.defaultMaxAge < std::chrono::seconds::zero() || config.maximumAge < std::chrono::seconds::zero()) {
    throw std::invalid_argument("response cache freshness durations must not be negative");
  }
  if ((config.methods & ~supportedMethods) != 0U || config.methods == 0U) {
    throw std::invalid_argument("response cache methods must contain only GET and/or HEAD");
  }
}

}  // namespace

HttpResponse ResponseCache::materialize(const Prototype& prototype, const HttpRequestView& request) {
  HttpResponse response = request.makeResponse(prototype.memoryCharge, prototype.status);
  if (!prototype.reason.empty()) {
    response.reason(prototype.reason);
  }
  _state.headerNamesScratch.clear();
  _state.headerNamesScratch.reserve(prototype.nbHeaders);
  for (const auto& header : prototype.headers) {
    const bool first = std::ranges::find(_state.headerNamesScratch, header.name) == _state.headerNamesScratch.end();
    const LowerAsciiKey key{header.name};
    if (first) {
      while (response.hasHeader(key)) {
        response.headerRemoveLine(key);
      }
      _state.headerNamesScratch.emplace_back(header.name);
    }
    response.headerAddLine(key, header.value);
  }
  if (prototype.sizeOnlyBody) {
    const std::string_view contentType =
        prototype.contentType.empty() ? http::ContentTypeTextPlain : prototype.contentType;
    response.setBodyHeaders(contentType, prototype.bodySize, HttpMessage::BodySetContext::Captured);
    response.setHeadSize(prototype.bodySize);
  } else if (prototype.hadBody) {
    response.body(prototype.body, prototype.contentType.empty() ? http::ContentTypeTextPlain : prototype.contentType);
  }
  return response;
}

ResponseCache::ResponseCache(ResponseCacheConfig config) : _state(std::move(config)) { ValidateConfig(_state.config); }

ResponseCache::ResponseCache(const ResponseCache& rhs) : _state(rhs._state.config) {}

ResponseCache& ResponseCache::operator=(const ResponseCache& rhs) {
  if (this != &rhs) {
    *this = ResponseCache(rhs._state.config);
  }
  return *this;
}

ResponseCache::~ResponseCache() = default;

ResponseCacheStats ResponseCache::stats() const noexcept {
  return {
      .hits = _state.hits,
      .misses = _state.misses,
      .stores = _state.stores,
      .replacements = _state.replacements,
      .evictions = _state.evictions,
      .expirations = _state.expirations,
      .bypasses = _state.bypasses,
      .currentEntries = _state.currentEntries,
      .currentMemoryBytes = _state.currentMemoryBytes,
  };
}

void ResponseCache::clear() noexcept {
  _state.entryPool.clear();
  _state.entries.clear();
  _state.newest = nullptr;
  _state.oldest = nullptr;
  _state.currentEntries = 0U;
  _state.currentMemoryBytes = 0U;
}

std::size_t ResponseCache::invalidatePath(std::string_view path) noexcept {
  std::size_t removed{};
  for (;;) {
    bool found = false;
    for (auto mapIt = _state.entries.begin(); mapIt != _state.entries.end() && !found; ++mapIt) {
      for (VariantIndex index = 0; index < mapIt->second.size(); ++index) {
        if (mapIt->second[index]->path == path) {
          RemoveAt(_state, mapIt, index, RemovalReason::None);
          ++removed;
          found = true;
          break;
        }
      }
    }
    if (!found) {
      return removed;
    }
  }
}

std::optional<HttpResponse> ResponseCache::lookup(const HttpRequestView& request) {
  if (!RequestMethodEligible(_state.config, request) || RequestHasSensitiveOrRangeHeaders(_state.config, request)) {
    ++_state.bypasses;
    return std::nullopt;
  }
  const CacheControlSummary requestControl = RequestCacheControl(request);
  if (requestControl.noStore || requestControl.noCache || requestControl.invalidFreshness ||
      requestControl.maxAge == 0U) {
    ++_state.bypasses;
    return std::nullopt;
  }

  BuildPrimaryKey(request, _state.primaryKeyScratch);
  const std::string_view primaryKey(_state.primaryKeyScratch);
  auto mapIt = _state.entries.find(primaryKey);
  const auto now = Clock::now();
  while (mapIt != _state.entries.end()) {
    bool removed = false;
    for (VariantIndex index = 0; index < mapIt->second.size(); ++index) {
      Entry* const entry = mapIt->second[index];
      if (entry->expiresAt <= now) {
        RemoveAt(_state, mapIt, index, RemovalReason::Expiration);
        mapIt = _state.entries.find(primaryKey);
        removed = true;
        break;
      }
      if (VaryMatches(*entry, request)) {
        Touch(_state, *entry);
        ++_state.hits;
        return materialize(entry->prototype, request);
      }
    }
    if (!removed) {
      break;
    }
  }
  ++_state.misses;
  return std::nullopt;
}

ResponseCache::StoreCandidate ResponseCache::capture(const HttpRequestView& request,
                                                     const HttpResponse& response) const {
  StoreCandidate ret;
  if (!RequestMethodEligible(_state.config, request) || RequestHasSensitiveOrRangeHeaders(_state.config, request) ||
      response.status() != http::StatusCodeOK || response.hasBodyFile() || response.trailersSize() != 0U ||
      (response.hasBody() && !response.hasBodyInMemory() && request.method() != http::Method::HEAD) ||
      response.bodyInMemorySize() > _state.config.maxEntryBytes) {
    return ret;
  }
  const CacheControlSummary requestControl = RequestCacheControl(request);
  if (requestControl.noStore) {
    return ret;
  }
  auto prototype = CapturePrototype(response);
  if (prototype.memoryCharge > _state.config.maxEntryBytes) {
    return ret;
  }
  ret._prototype = std::move(prototype);
  return ret;
}

void ResponseCache::commit(const HttpRequestView& request, StoreCandidate candidate, const HttpResponse& finalResponse,
                           const ConcatenatedHeaders* globalHeaders) {
  if (!candidate) {
    return;
  }

  BuildPrimaryKey(request, _state.primaryKeyScratch);
  const std::string_view primaryKey(_state.primaryKeyScratch);
  auto invalidatePrimary = [&] { (void)RemovePrimary(_state, primaryKey); };
  std::string_view ignoredHeader;
  if (finalResponse.status() != http::StatusCodeOK || finalResponse.hasBodyFile() ||
      finalResponse.trailersSize() != 0U ||
      (_state.config.bypassSetCookie &&
       (finalResponse.hasHeader(http::SetCookie) ||
        FirstEffectiveHeader(finalResponse, globalHeaders, http::SetCookie, ignoredHeader)))) {
    invalidatePrimary();
    ++_state.bypasses;
    return;
  }

  const CacheControlSummary responseControl = EffectiveCacheControl(finalResponse, globalHeaders);
  const std::chrono::seconds lifetime = FreshnessLifetime(_state.config, responseControl);
  if (lifetime == std::chrono::seconds::zero()) {
    invalidatePrimary();
    ++_state.bypasses;
    return;
  }

  if (!EffectiveVaryNames(finalResponse, globalHeaders, _state.varyNamesScratch)) {
    invalidatePrimary();
    ++_state.bypasses;
    return;
  }
  VariantIdentity identity = CaptureVariantIdentity(request, _state.varyNamesScratch, _state);
  const std::size_t charge = EntryCharge(_state.primaryKeyScratch, identity, candidate._prototype);
  if (charge > _state.config.maxEntryBytes || charge > _state.config.maxMemoryBytes) {
    invalidatePrimary();
    ++_state.bypasses;
    return;
  }

  const auto now = Clock::now();
  const auto maxDelta = Clock::time_point::max() - now;
  const auto expiresAt = lifetime >= std::chrono::duration_cast<std::chrono::seconds>(maxDelta)
                             ? Clock::time_point::max()
                             : now + lifetime;

  auto mapIt = _state.entries.find(primaryKey);
  while (mapIt != _state.entries.end()) {
    const auto differentSchema = std::ranges::find_if(
        mapIt->second, [&](const Entry* existing) { return !SameVarySchema(*existing, identity.fields); });
    if (differentSchema == mapIt->second.end()) {
      break;
    }
    (void)RemoveEntry(_state, *differentSchema, RemovalReason::None);
    mapIt = _state.entries.find(primaryKey);
  }

  if (mapIt != _state.entries.end()) {
    for (VariantIndex index = 0; index < mapIt->second.size(); ++index) {
      if (SameVariant(*mapIt->second[index], identity.fields)) {
        RemoveAt(_state, mapIt, index, RemovalReason::None);
        ++_state.replacements;
        break;
      }
    }
  }

  mapIt = _state.entries.find(primaryKey);
  if (mapIt != _state.entries.end() && mapIt->second.size() >= _state.config.maxVariantsPerTarget) {
    const auto oldestVariant =
        std::ranges::min_element(mapIt->second, {}, [](const Entry* item) { return item->accessSequence; });
    (void)RemoveEntry(_state, *oldestVariant, RemovalReason::Eviction);
  }

  while (_state.currentEntries >= _state.config.maxEntries ||
         charge > _state.config.maxMemoryBytes - _state.currentMemoryBytes) {
    if (!RemoveEntry(_state, _state.oldest, RemovalReason::Eviction)) {
      break;
    }
  }

  Entry* entry =
      _state.entryPool.allocateAndConstruct(std::move(identity), std::move(candidate._prototype), expiresAt, charge);
  bool insertedPrimary = false;
  try {
    mapIt = _state.entries.find(primaryKey);
    if (mapIt == _state.entries.end()) {
      auto result = _state.entries.try_emplace(RawChars32(primaryKey));
      mapIt = result.first;
      insertedPrimary = result.second;
    }
    entry->primaryKey = std::string_view(mapIt->first);
    mapIt->second.emplace_back(entry);
  } catch (...) {
    if (insertedPrimary) {
      _state.entries.erase(mapIt);
    }
    _state.entryPool.destroyAndRelease(entry);
    throw;
  }

  AttachNewest(_state, *entry);
  ++_state.currentEntries;
  _state.currentMemoryBytes += charge;
  ++_state.stores;
}

void ResponseCache::applyConditional(const HttpRequestView& request, HttpResponse& finalResponse,
                                     const ConcatenatedHeaders* globalHeaders) {
  const auto condition = request.headerValue(http::IfNoneMatch);
  std::string_view entityTag;
  if (!condition || !FirstEffectiveHeader(finalResponse, globalHeaders, http::ETag, entityTag) ||
      !IfNoneMatchMatches(*condition, entityTag)) {
    return;
  }

  HttpResponse notModified = request.makeResponse(http::StatusCodeNotModified);
  _state.headerNamesScratch.clear();
  for (const auto [name, value] : finalResponse.headers()) {
    if (!IsConditionalMetadata(name) ||
        std::ranges::find(_state.headerNamesScratch, name) != _state.headerNamesScratch.end()) {
      continue;
    }
    CopyHeaderReplacingGlobals(notModified, finalResponse, name);
    _state.headerNamesScratch.emplace_back(name);
  }
  finalResponse = std::move(notModified);
}

}  // namespace aeronet
