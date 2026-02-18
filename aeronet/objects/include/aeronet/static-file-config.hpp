#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/static-concatenated-strings.hpp"

namespace aeronet {
/// Configuration knobs for StaticFileHandler (serving filesystem trees).
class StaticFileConfig {
 public:
  void validate() const;

  /// Name of the file served when the target path resolves to a directory.
  [[nodiscard]] std::string_view defaultIndex() const noexcept { return _staticFileStrings[0]; }

  /// Content-Type header value used when the following conditions are met:
  ///  - no contentTypeResolver is provided or it returns empty (for a given path)
  ///  - the file extension is from unknown MIME type
  /// Default is "application/octet-stream".
  /// It cannot be empty.
  [[nodiscard]] std::string_view defaultContentType() const noexcept { return _staticFileStrings[1]; }

  // Optional CSS stylesheet for directory listings.
  [[nodiscard]] std::string_view directoryListingCss() const noexcept { return _staticFileStrings[2]; }

  StaticFileConfig &withDefaultIndex(std::string_view indexFile) {
    _staticFileStrings.set(0, indexFile);
    return *this;
  }

  /// Content-Type header value used when the following conditions are met:
  ///  - no contentTypeResolver is provided or it returns empty (for a given path)
  ///  - the file extension is from unknown MIME type
  /// Default is "application/octet-stream".
  /// It cannot be empty.
  StaticFileConfig &withDefaultContentType(std::string_view contentType) {
    _staticFileStrings.set(1, contentType);
    return *this;
  }

  StaticFileConfig &withDirectoryListingCss(std::string_view cssFile) {
    _staticFileStrings.set(2, cssFile);
    return *this;
  }

  StaticFileConfig &withInlineFileThresholdBytes(std::size_t threshold) {
    inlineFileThresholdBytes = threshold;
    return *this;
  }

  /// Whether byte-range requests are honored (RFC 7233 single range).
  bool enableRange{true};

  // Whether conditional headers (ETag, If-* preconditions) are processed.
  bool enableConditional{true};

  // Emit Last-Modified header when metadata is available.
  bool addLastModified{true};

  // Emit a strong ETag derived from file size and modification time.
  bool addEtag{true};

  // Whether directory index requests are allowed (i.e., serving defaultIndex file).
  bool enableDirectoryIndex{false};

  // Whether hidden files (dotfiles) are served.
  bool showHiddenFiles{false};

  /// Optional callback returning Content-Type for the resolved file path.
  /// Warning: the returned string_view must point to valid memory at all times (for instance to constant storage).
  /// The callback can return an empty string_view to fallback to automatic MIME type resolution, and then to
  /// defaultContentType() if unknown.
  std::function<std::string_view(std::string_view)> contentTypeResolver;

  /// Optional callback to render directory index HTML.
  std::function<std::string(const std::filesystem::path &directory, std::span<const std::filesystem::directory_entry>)>
      directoryIndexRenderer;

  /// Files smaller than this threshold are read into the response body (inline) rather than being served via
  /// the zero-copy transport path (sendfile in Linux).
  /// Set to 0 to disable the optimization, or set to max to always read files into memory.
  /// Default: 128 KiB.
  std::size_t inlineFileThresholdBytes = 128UL * 1024;

  /// guard against pathological directories (configurable)
  std::size_t maxEntriesToList = 10000;

 private:
  StaticConcatenatedStrings<3, uint32_t> _staticFileStrings{"index.html", http::ContentTypeApplicationOctetStream,
                                                            std::string_view()};
};

}  // namespace aeronet
