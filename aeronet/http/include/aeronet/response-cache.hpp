#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "aeronet/city-hash.hpp"
#include "aeronet/concatenated-headers.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

class HttpRequestView;

namespace internal {
struct ResponseCachePrototype;
}  // namespace internal

/// Configuration for a per-route in-memory server response cache.
///
/// The cache is deliberately conservative: only buffered 200 GET/HEAD responses are
/// eligible, authenticated/ranged requests and Set-Cookie responses are bypassed by default,
/// and responses without an explicit freshness lifetime are only stored when defaultMaxAge is
/// non-zero.
struct ResponseCacheConfig {
  std::size_t maxEntries{1024};
  std::size_t maxMemoryBytes{64UL * 1024U * 1024U};
  std::size_t maxEntryBytes{8UL * 1024U * 1024U};
  std::size_t maxVariantsPerTarget{16};
  std::chrono::seconds defaultMaxAge{0};
  std::chrono::seconds maximumAge{std::chrono::seconds::max()};
  http::MethodBmp methods{http::Method::GET | http::Method::HEAD};
  bool bypassAuthorization{true};
  bool bypassCookie{true};
  bool bypassSetCookie{true};
};

struct ResponseCacheStats {
  uint64_t hits{};
  uint64_t misses{};
  uint64_t stores{};
  uint64_t replacements{};
  uint64_t evictions{};
  uint64_t expirations{};
  uint64_t bypasses{};
  std::size_t currentEntries{};
  std::size_t currentMemoryBytes{};
};

namespace internal {

struct ResponseCachePrototype {
  http::StatusCode status{};
  bool hadBody{};
  bool sizeOnlyBody{};
  uint32_t nbHeaders{};
  std::string_view reason;
  HeadersView headers;
  std::string_view contentType;
  std::string_view body;
  std::size_t bodySize{};
  std::size_t memoryCharge{};
  std::unique_ptr<char[]> buf;
};

struct ResponseCacheVaryField {
  std::string_view name;
  // nullptr distinguishes an absent request header from a present empty header.
  std::string_view value;
};

struct ResponseCacheVariantIdentity {
  RawChars32 strings;
  vector<ResponseCacheVaryField> fields;
  std::string_view path;
};

struct ResponseCacheEntry {
  ResponseCacheEntry(ResponseCacheVariantIdentity identity, ResponseCachePrototype prototype,
                     std::chrono::steady_clock::time_point expiresAt, std::size_t memoryCharge) noexcept
      : path(identity.path),
        strings(std::move(identity.strings)),
        varyFields(std::move(identity.fields)),
        prototype(std::move(prototype)),
        expiresAt(expiresAt),
        memoryCharge(memoryCharge) {}

  // The map owns this view's storage. RawChars32 moves do not move its characters.
  std::string_view primaryKey;
  std::string_view path;
  RawChars32 strings;
  vector<ResponseCacheVaryField> varyFields;
  ResponseCachePrototype prototype;
  std::chrono::steady_clock::time_point expiresAt;
  std::size_t memoryCharge{};
  uint64_t accessSequence{};
  ResponseCacheEntry* newer{};
  ResponseCacheEntry* older{};
};

struct ResponseCacheState {
  explicit ResponseCacheState(ResponseCacheConfig cfg) : config(std::move(cfg)) {}

  ResponseCacheConfig config;
  flat_hash_map<RawChars32, vector<ResponseCacheEntry*>, CityHash, std::equal_to<>> entries;
  ObjectPool<ResponseCacheEntry> entryPool;
  ResponseCacheEntry* newest{};
  ResponseCacheEntry* oldest{};
  std::size_t currentEntries{};
  std::size_t currentMemoryBytes{};
  uint64_t accessSequence{};
  uint64_t hits{};
  uint64_t misses{};
  uint64_t stores{};
  uint64_t replacements{};
  uint64_t evictions{};
  uint64_t expirations{};
  uint64_t bypasses{};

  // Transient request-processing storage. Each cache belongs to one SingleHttpServer thread.
  RawChars32 primaryKeyScratch;
  RawChars32 normalizedVaryNamesScratch;
  vector<std::string_view> varyNamesScratch;
  vector<std::string_view> normalizedVaryNameViewsScratch;
  vector<std::string_view> headerNamesScratch;
};

}  // namespace internal

/// A bounded in-memory response cache owned by one route in one SingleHttpServer.
///
/// Copying a cache copies its configuration and starts with empty entries and statistics. Router
/// copies therefore give every server thread an independent cache without synchronization.
class ResponseCache {
 public:
  class StoreCandidate {
   public:
    StoreCandidate() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept { return _prototype.status != 0; }

   private:
    friend class ResponseCache;

    internal::ResponseCachePrototype _prototype;
  };

  explicit ResponseCache(ResponseCacheConfig config = {});

  ResponseCache(const ResponseCache& rhs);
  ResponseCache& operator=(const ResponseCache& rhs);
  ResponseCache(ResponseCache&&) noexcept = default;
  ResponseCache& operator=(ResponseCache&&) noexcept = default;

  ~ResponseCache();

  [[nodiscard]] const ResponseCacheConfig& config() const noexcept { return _state.config; }
  [[nodiscard]] ResponseCacheStats stats() const noexcept;

  /// Remove all entries.
  void clear() noexcept;

  /// Remove all cached variants whose decoded request path exactly equals path.
  /// Host, method, and query variations are all invalidated.
  [[nodiscard]] std::size_t invalidatePath(std::string_view path) noexcept;

  // Dispatch integration API. These methods are public so custom transports can preserve the
  // same cache lifecycle; ordinary applications normally only construct and attach the cache.
  // The transient optional avoids allocating a default HttpResponse on every cache miss; no
  // optional is stored in the cache or attached to a route.
  [[nodiscard]] std::optional<HttpResponse> lookup(const HttpRequestView& request);

  [[nodiscard]] StoreCandidate capture(const HttpRequestView& request, const HttpResponse& response) const;

  void commit(const HttpRequestView& request, StoreCandidate candidate, const HttpResponse& finalResponse,
              const ConcatenatedHeaders* globalHeaders = nullptr);

  void applyConditional(const HttpRequestView& request, HttpResponse& finalResponse,
                        const ConcatenatedHeaders* globalHeaders = nullptr);

 private:
  HttpResponse materialize(const internal::ResponseCachePrototype& prototype, const HttpRequestView& request);

  internal::ResponseCacheState _state;
};

}  // namespace aeronet
