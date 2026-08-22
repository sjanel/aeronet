#pragma once

#include <cstdint>
#include <string_view>

namespace aeronet {

// Packs up to kMaxChars bytes of a file extension into a single uint64_t,
// case-folding ASCII letters to lowercase along the way (digits already have
// bit 0x20 set, so they're untouched by the fold).
//
// Characters are placed big-endian (first char = most significant byte), and
// unused trailing bytes are padded with 0x20 (space), which is numerically
// below any digit or letter. Consequence: comparing two MIMEExtensionCode
// values numerically gives EXACTLY the same result as comparing the original
// (lowercased) extension strings lexicographically, including "shorter
// prefix sorts first" (e.g. "js" < "json"). So a table written in plain
// alphabetical order is automatically sorted by MIMEExtensionCode -- one
// array serves as both the readable source and the binary-search table.
class MIMEExtensionCode {
 public:
  static constexpr auto kMaxChars = sizeof(uint64_t);  // current longest known: 5 ("woff2", "pjpeg")

  constexpr explicit MIMEExtensionCode(std::string_view ext) : _code(Pack(ext)) {}

  // Implicit on purpose: lets MIMEMapping's initializer list stay written as
  // plain string literals, e.g. {"7z", "application/x-7z-compressed"}.
  template <unsigned N>
    requires(N <= kMaxChars + 1U)
  constexpr MIMEExtensionCode(const char (&ext)[N]) : MIMEExtensionCode(std::string_view(ext, N - 1U)) {}

  constexpr bool operator==(const MIMEExtensionCode&) const = default;
  constexpr auto operator<=>(const MIMEExtensionCode&) const = default;

 private:
  static constexpr uint64_t Pack(std::string_view ext) {
    uint64_t code = 0;
    for (std::string_view::size_type charIdx = 0; charIdx < ext.size(); ++charIdx) {
      code |= static_cast<uint64_t>(static_cast<unsigned char>(ext[charIdx])) << (8U * (kMaxChars - 1U - charIdx));
    }
    // lower case ASCII letters
    return code | 0x2020202020202020ULL;
  }

  uint64_t _code{};
};

struct MIMEMapping {
  MIMEExtensionCode extensionCode;
  std::string_view mimeType;
};

using MIMETypeIdx = uint8_t;

inline constexpr MIMETypeIdx kUnknownMIMEMappingIdx = static_cast<MIMETypeIdx>(~0);

// Single source of truth, written in plain alphabetical order for
// readability -- this order is also required for binary search on
// extensionCode (enforced by static_assert in the .cpp).
inline constexpr MIMEMapping kMIMEMappings[]{
    {"7z", "application/x-7z-compressed"},
    {"aac", "audio/aac"},
    {"apng", "image/apng"},
    {"avi", "video/x-msvideo"},
    {"avif", "image/avif"},
    {"bmp", "image/bmp"},
    {"c", "text/x-csrc"},
    {"cc", "text/x-c++src"},
    {"cpp", "text/x-c++src"},
    {"css", "text/css"},
    {"csv", "text/csv"},
    {"doc", "application/msword"},
    {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {"exe", "application/vnd.microsoft.portable-executable"},
    {"flac", "audio/flac"},
    {"gif", "image/gif"},
    {"gz", "application/gzip"},
    {"h", "text/x-chdr"},
    {"hpp", "text/x-c++hdr"},
    {"htm", "text/html"},
    {"html", "text/html"},
    {"ico", "image/x-icon"},
    {"jfif", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"jpg", "image/jpeg"},
    // Per IETF RFC 9239, `text/javascript` is the recommended media type for
    // JavaScript source; `application/javascript` is now considered obsolete.
    {"js", "text/javascript"},
    {"json", "application/json"},
    {"m4a", "audio/mp4"},
    {"m4v", "video/x-m4v"},
    {"map", "application/json"},
    {"md", "text/markdown"},
    {"mjs", "text/javascript"},
    {"mov", "video/quicktime"},
    {"mp3", "audio/mpeg"},
    {"mp4", "video/mp4"},
    {"mpeg", "video/mpeg"},
    {"mpg", "video/mpeg"},
    {"oga", "audio/ogg"},
    {"ogg", "audio/ogg"},
    {"otf", "font/otf"},
    {"pdf", "application/pdf"},
    {"pjp", "image/jpeg"},
    {"pjpeg", "image/jpeg"},
    {"png", "image/png"},
    {"ppt", "application/vnd.ms-powerpoint"},
    {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {"ps1", "text/plain"},
    {"py", "text/x-python"},
    {"rar", "application/vnd.rar"},
    {"rss", "application/rss+xml"},
    {"sh", "application/x-sh"},
    {"svg", "image/svg+xml"},
    {"tar", "application/x-tar"},
    {"tgz", "application/gzip"},
    {"tif", "image/tiff"},
    {"tiff", "image/tiff"},
    {"ttf", "font/ttf"},
    {"txt", "text/plain"},
    {"wasm", "application/wasm"},
    {"webm", "video/webm"},
    {"webp", "image/webp"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"xls", "application/vnd.ms-excel"},
    {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {"xml", "application/xml"},
    {"zip", "application/zip"},
};

// Given a file path, determine the appropriate MIME type mapping index, if known.
// This function is non-allocating, and case insensitive for the extension.
// Otherwise, returns kUnknownMIMEMappingIdx.
MIMETypeIdx DetermineMIMETypeIdx(std::string_view path);

// Given a file path, determine the appropriate MIME type string, if known.
// This function is non-allocating, and case insensitive for the extension.
// Otherwise, returns an empty string_view.
std::string_view DetermineMIMETypeStr(std::string_view path);

}  // namespace aeronet