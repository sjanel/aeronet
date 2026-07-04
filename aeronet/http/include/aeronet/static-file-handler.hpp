#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>

#include "aeronet/city-hash.hpp"
#include "aeronet/file.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
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

  /// Build a response for the given request. Only GET and HEAD are served.
  [[nodiscard]] HttpResponse operator()(const HttpRequest& request) const;

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

  [[nodiscard]] ResolveResult resolveTarget(const HttpRequest& request, std::filesystem::path& resolvedPath) const;

  // Format the per-file header fragments (ETag, Last-Modified, Content-Type) for 'file' into 'out'.
  void buildHeaderMeta(std::string_view filePath, const File& file, CachedFileHeaders& out) const;

  // Return the per-file header fragments, reusing a cached entry when the file is unchanged, otherwise (re)building
  // them and inserting them into the cache (evicting the least-recently-used entry when it exceeds capacity).
  // When caching is disabled, 'scratch' is used as backing storage for the returned value.
  [[nodiscard]] const CachedFileHeaders& resolveHeaderMeta(std::string_view filePath, const File& file,
                                                           CachedFileHeaders& scratch) const;

  friend class StaticFileHandlerTest;

  std::filesystem::path _root;
  StaticFileConfig _config;
  // Keyed by the resolved file path. Heterogeneous lookup (CityHash + std::equal_to<>) keeps request-time lookups
  // allocation-free; a RawChars32 key is only materialized when a new entry is inserted.
  mutable flat_hash_map<RawChars32, CachedFileHeaders, CityHash, std::equal_to<>> _headerCache;
  // Monotonically increasing "logical clock" stamped onto an entry each time it is used, so the entry with the
  // smallest stamp is the least-recently used. Never wraps in practice (2^64 accesses per handler instance).
  mutable std::uint64_t _headerCacheClock{0};
};

}  // namespace aeronet
