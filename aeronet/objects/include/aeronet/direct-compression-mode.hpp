#pragma once

#include <cstdint>

namespace aeronet {

// Controls inline (direct) compression behavior for HttpResponse.
//
// Direct compression applies to inline bodies created via HttpRequest::makeResponse(), and compresses data as it is
// written via body() / bodyAppend(), before finalization.
//
// This avoids a second compression pass and temporary buffers for eligible responses.
enum class DirectCompressionMode : std::uint8_t {

  // Enable direct compression when:
  //  • The request contains a supported Accept-Encoding
  //  • No user-supplied Content-Encoding header is present
  //  • The body is inline (not captured or file-backed)
  //  • The first body chunk size >= CompressionConfig::minBytes
  //  • The content type matches CompressionConfig::contentTypeAllowList
  //
  // Compression starts immediately on the first eligible body write.
  Auto,

  // Disable direct compression entirely.
  //
  // Automatic compression may still occur at finalization if enabled globally.
  Off,

  // Force direct compression whenever Accept-Encoding permits, bypassing minBytes and content-type checks.
  //
  // Still requires a supported Accept-Encoding header.
  On
};

}  // namespace aeronet