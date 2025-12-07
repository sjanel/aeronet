#pragma once

#include <cstdint>
#include <string_view>

namespace aeronet {

struct MIMEMapping {
  std::string_view extension;
  std::string_view mimeType;
};

using MIMETypeIdx = uint8_t;

inline constexpr MIMETypeIdx kUnknownMIMEMappingIdx = static_cast<MIMETypeIdx>(~0);

inline constexpr MIMEMapping kMIMEMappings[] = {
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