#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace aeronet {

/// Configuration knobs for StaticFileHandler (serving filesystem trees).
struct StaticFileConfig {
  void validate() const;

  /// Name of the file served when the target path resolves to a directory.
  std::string defaultIndex{"index.html"};
  /// Whether byte-range requests are honored (RFC 7233 single range).
  bool enableRange{true};
  /// Whether conditional headers (ETag, If-* preconditions) are processed.
  bool enableConditional{true};
  /// Emit Last-Modified header when metadata is available.
  bool addLastModified{true};
  /// Emit a strong ETag derived from file size and modification time.
  bool addEtag{true};
  /// Default MIME type when no resolver is provided.
  std::string defaultContentType{"application/octet-stream"};
  /// Optional callback returning Content-Type for the resolved file path.
  std::function<std::string(std::string_view)> contentTypeResolver;
};

}  // namespace aeronet
