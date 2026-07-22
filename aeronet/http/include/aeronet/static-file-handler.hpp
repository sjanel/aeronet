#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>

#include "aeronet/city-hash.hpp"
#include "aeronet/file.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/static-file-config.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

// Serves files from a fixed root directory with RFC 7233 / RFC 7232 semantics.
// Can be used as a RequestHandler callable handler in Router.
class StaticFileHandler {
 public:
  explicit StaticFileHandler(std::filesystem::path rootDirectory, StaticFileConfig config = {});

  StaticFileHandler(const StaticFileHandler& rhs);
  StaticFileHandler(StaticFileHandler&&) noexcept = default;
  StaticFileHandler& operator=(const StaticFileHandler& rhs);
  StaticFileHandler& operator=(StaticFileHandler&&) noexcept = default;

  /// Build a response for the given request. Only GET and HEAD are served.
  [[nodiscard]] HttpResponse operator()(const HttpRequestView& request) const;

 private:
  enum class ResolveResult : uint8_t { NotFound, RegularFile, Directory };

  // Maximum size of a strong ETag value ("<hex-size>-<hex-nanos>"), quotes included.
  static constexpr std::size_t kMaxHexChars = sizeof(std::uint64_t) * 2;
  static constexpr std::size_t kMaxEtagSize = 1 + kMaxHexChars + 1 + kMaxHexChars + 1;

  // Pre-formatted, per-file response header fragments cached alongside the metadata used to invalidate them.
  // Storing the already-formatted bytes lets repeated requests to the same (unchanged) file skip the ETag, date
  // and Content-Type formatting entirely. Field order minimizes padding.
  struct CachedFileHeaders {
    [[nodiscard]] std::string_view contentType() const { return {pContentType, contentTypeLen}; }

    const char* pContentType{};                    // stable storage (MIME table / resolver / config default)
    SysTimePoint lastModified{kInvalidTimePoint};  // also the mtime validator; drives conditional comparisons
    std::size_t fileSize{0};                       // size validator
    std::uint64_t lruSeq{0};                  // recency stamp; the smallest one is evicted first when the cache is full
    std::uint32_t contentTypeLen{0};          // length of the MIME type string
    char etag[kMaxEtagSize];                  // formatted strong ETag (quotes included), etagLen bytes used
    char lastModifiedStr[RFC7231DateStrLen];  // formatted RFC 7231 date, valid iff lastModified is valid
    std::uint8_t etagLen{0};                  // 0 means no ETag
  };

  [[nodiscard]] ResolveResult resolveTarget(const HttpRequestView& request, std::filesystem::path& resolvedPath) const;

  // Format the per-file header fragments (ETag, Last-Modified, Content-Type) for 'file' into 'out'.
  void buildHeaderMeta(std::string_view filePath, const File& file, CachedFileHeaders& out) const;

  // Return the per-file header fragments, reusing a cached entry when the file is unchanged, otherwise (re)building
  // them and inserting them into the cache (evicting the least-recently-used entry when it exceeds capacity).
  // When caching is disabled, 'scratch' is used as backing storage for the returned value.
  [[nodiscard]] const CachedFileHeaders& resolveHeaderMeta(std::string_view filePath, const File& file,
                                                           CachedFileHeaders& scratch) const;

  // One cached entry plus its LRU-list linkage. Allocated from a pool that provides pointer-stability across
  // growth, so the map value is a plain CacheNode* and the LRU links are plain CacheNode* too - no index
  // indirection needed. The *map* itself may still relocate elements on erase()/rehash (open addressing), but
  // it only ever stores a pointer to the node, never the node itself, so that relocation never touches this data.
  struct CacheNode {
    CachedFileHeaders headers;
    RawChars32 key;             // needed to erase the map entry when this node is evicted
    CacheNode* pPrev{nullptr};  // neighbor towards the MRU head
    CacheNode* pNext{nullptr};  // neighbor towards the LRU tail
  };

  void lruUnlink(CacheNode* pNode) const;
  void lruPushFront(CacheNode* pNode) const;

  friend class StaticFileHandlerTest;

  std::filesystem::path _root;
  StaticFileConfig _config;

  mutable flat_hash_map<RawChars32, CacheNode*, CityHash, std::equal_to<>> _headerCache;
  mutable ObjectPool<CacheNode> _headerCachePool;
  mutable CacheNode* _lruHead{nullptr};  // most-recently-used
  mutable CacheNode* _lruTail{nullptr};  // least-recently-used (eviction victim)
};

}  // namespace aeronet
