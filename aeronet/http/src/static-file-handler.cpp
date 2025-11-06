#include "aeronet/static-file-handler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/static-file-config.hpp"
#include "cctype.hpp"
#include "file.hpp"
#include "log.hpp"
#include "ndigits.hpp"
#include "raw-chars.hpp"
#include "string-equal-ignore-case.hpp"
#include "stringconv.hpp"
#include "timedef.hpp"
#include "timestring.hpp"
#include "url-encode.hpp"
#include "vector.hpp"

namespace aeronet {
namespace {

[[nodiscard]] bool isHiddenName(std::string_view name) { return !name.empty() && name.front() == '.'; }

// Use a constexpr lookup table indexed by unsigned char for a fast, branchless test.
// This avoids multiple comparisons and gives the compiler a chance to emit a single
// table-lookup instruction. We use unsigned char to index the array safely.
constexpr auto kUnreservedTable = []() constexpr {
  std::array<bool, std::numeric_limits<unsigned char>::max() + 1> table{};

  std::ranges::fill(table.begin() + 'A', table.begin() + 'Z' + 1, true);
  std::ranges::fill(table.begin() + 'a', table.begin() + 'z' + 1, true);
  std::ranges::fill(table.begin() + '0', table.begin() + '9' + 1, true);

  table[static_cast<unsigned char>('-')] = true;
  table[static_cast<unsigned char>('_')] = true;
  table[static_cast<unsigned char>('.')] = true;
  table[static_cast<unsigned char>('~')] = true;

  return table;
}();

void appendHtmlEscaped(std::string_view requestPath, RawChars& out) {
  for (char ch : requestPath) {
    switch (ch) {
      case '&':
        out.append("&amp;");
        break;
      case '<':
        out.append("&lt;");
        break;
      case '>':
        out.append("&gt;");
        break;
      case '"':
        out.append("&quot;");
        break;
      case '\'':
        out.append("&#39;");
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
}

// Format a file size into a human-readable string using binary units (powers of 1024).
// Rules:
//  - Units used: B, KB, MB, GB, TB (where 1 KB == 1024 bytes, 1 MB == 1024*1024 bytes, ...).
//  - For values < 1024 bytes the function prints an integer number of bytes, e.g. "512 B".
//  - For values >= 1024 the value is divided by 1024 repeatedly to find the largest unit
//    with a value < 1024. For those units we print a decimal number; formatting uses one
//    decimal place when the numeric value is less than 10 (to preserve a single significant
//    fractional digit) and no decimals when the value is >= 10. Examples:
//      0         -> "0 B"
//      512       -> "512 B"
//      1536      -> "1.5 KB"   (1536 / 1024 == 1.5)
//      1048576   -> "1.0 MB"   (1024*1024 -> value is 1.0, one decimal is shown)
//      12345678  -> "11.8 MB"  (approx; displays one decimal when < 10, otherwise no fractional)
//  - A single space separates the number and the unit (e.g. "1.5 KB").
void addFormattedSize(std::uintmax_t size, RawChars& out) {
  static constexpr std::array<std::string_view, 5> units{"B", "KB", "MB", "GB", "TB"};

  // Find the largest unit where the value is < 1024 (binary units, 1024^n)
  std::size_t unitIdx = 0;
  std::uintmax_t divisor = 1;
  for (; unitIdx + 1U < units.size() && size >= divisor * 1024U; ++unitIdx) {
    divisor *= 1024U;
  }

  // small helper: append integer value and the unit (with leading space)
  const auto appendIntAndUnit = [&out](std::uintmax_t value, std::string_view unit) {
    const auto buf = IntegralToCharVector(value);
    out.ensureAvailableCapacity(buf.size() + 1 + unit.size());
    out.unchecked_append(buf.data(), buf.size());
    out.unchecked_push_back(' ');
    out.unchecked_append(unit);
  };

  // Bytes: print integer bytes
  if (unitIdx == 0U) {
    appendIntAndUnit(size, units[unitIdx]);
    return;
  }

  // For units >= KB, follow existing formatting rules: if the numeric value is < 10, print one
  // decimal place (rounded). Otherwise print an integer (rounded).
  // Check value < 10  <=> size < divisor * 10
  if (size < divisor * 10U) {
    const std::uintmax_t intPart = size / divisor;
    const std::uintmax_t rem = size % divisor;
    // frac10 = round(rem * 10 / divisor)
    const std::uintmax_t frac10 = (rem * 10U + divisor / 2U) / divisor;
    std::uintmax_t finalInt = intPart;
    std::uintmax_t finalFrac = frac10;
    if (frac10 >= 10U) {
      // carry into integer part (e.g. 9.96 -> rounds to 10.0)
      finalInt = intPart + 1U;
      finalFrac = 0U;
    }
    // If carry produced a value >= 10 we should print integer form (no decimal) per rules
    if (finalInt >= 10U) {
      appendIntAndUnit(finalInt, units[unitIdx]);
      return;
    }
    // print one decimal: int.frac unit
    const auto intBuf = IntegralToCharVector(finalInt);
    const auto fracBuf = IntegralToCharVector(finalFrac);
    out.ensureAvailableCapacity(intBuf.size() + 1U + fracBuf.size() + 1U + units[unitIdx].size());
    out.unchecked_append(intBuf.data(), intBuf.size());
    out.unchecked_push_back('.');
    out.unchecked_append(fracBuf.data(), fracBuf.size());
    out.unchecked_push_back(' ');
    out.unchecked_append(units[unitIdx]);
    return;
  }

  // Print integer with rounding
  const std::uintmax_t rounded = (size + divisor / 2U) / divisor;
  appendIntAndUnit(rounded, units[unitIdx]);
}

void formatLastModified(SysTimePoint tp, RawChars& buf) {
  if (tp == kInvalidTimePoint) {
    buf.push_back('-');
  } else {
    buf.ensureAvailableCapacity(kRFC7231DateStrLen);
    TimeToStringRFC7231(tp, buf.data() + buf.size());
    buf.addSize(kRFC7231DateStrLen);
  }
}

void defaultDirectoryListingCss(std::string_view customCss, RawChars& buf) {
  if (!customCss.empty()) {
    buf.append(customCss);
  } else {
    buf.append(R"CSS(
body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:2rem;}
table{border-collapse:collapse;width:100%;max-width:960px;}
th,td{padding:0.3rem 0.6rem;text-align:left;border-bottom:1px solid #e0e0e0;}
tbody tr:hover{background:#f8f8f8;}
td.size,td.modified{text-align:right;font-variant-numeric:tabular-nums;}
h1{font-size:1.4rem;margin-bottom:1rem;}
#truncated{margin-top:1rem;color:#b24e00;}
footer{margin-top:2rem;font-size:0.85rem;color:#666;}
a.dir::after{content:"/";}
)CSS");
  }
}

struct DirectoryListingEntry {
  std::string name;
  bool isDirectory{false};
  bool sizeKnown{false};
  std::uintmax_t sizeBytes{0};
  SysTimePoint lastModified{kInvalidTimePoint};
  std::filesystem::directory_entry entry;
};

[[nodiscard]] RawChars renderDefaultDirectoryListing(std::string_view requestPath,
                                                     std::span<const DirectoryListingEntry> entries, bool truncated,
                                                     std::string_view customCss) {
  RawChars body(2048U);

  body.unchecked_append("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<title>Index of ");
  appendHtmlEscaped(requestPath, body);
  body.append("</title>\n<style>");

  defaultDirectoryListingCss(customCss, body);

  body.append("</style>\n</head>\n<body>\n<h1>Index of ");
  appendHtmlEscaped(requestPath, body);
  body.append(
      "</h1>\n<table>\n<thead><tr><th>Name</th><th class=\"size\">Size</th><th class=\"modified\">Last "
      "Modified</th></tr></thead>\n<tbody>\n");

  if (!requestPath.empty()) {
    body.append(
        "<tr><td class=\"name\"><a href=\"../\" class=\"dir\">..</a></td><td class=\"size\">-</td><td "
        "class=\"modified\">-</td></tr>\n");
  }

  for (const auto& entry : entries) {
    const bool isDir = entry.isDirectory;
    const auto encodedSz =
        URLEncodedSize(entry.name, [](char ch) { return kUnreservedTable[static_cast<unsigned char>(ch)]; });

    body.append(R"(<tr><td class="name"><a href=")");

    body.ensureAvailableCapacity(encodedSz);
    URLEncode(
        entry.name, [](char ch) { return kUnreservedTable[static_cast<unsigned char>(ch)]; },
        body.data() + body.size());
    body.addSize(encodedSz);

    if (isDir) {
      // ensure href ends with '/'
      body.push_back('/');
    }
    body.push_back('"');
    if (isDir) {
      body.append(" class=\"dir\"");
    }
    body.push_back('>');
    appendHtmlEscaped(entry.name, body);

    body.append("</a></td><td class=\"size\">");
    if (entry.sizeKnown && !isDir) {
      addFormattedSize(entry.sizeBytes, body);
    } else {
      body.push_back('-');
    }
    body.append("</td><td class=\"modified\">");
    formatLastModified(entry.lastModified, body);
    body.append("</td></tr>\n");
  }

  body.append("</tbody>\n</table>\n");
  if (truncated) {
    body.append("<p id=\"truncated\">Listing truncated after ");
    body.append(std::string_view(IntegralToCharVector(entries.size())));
    body.append(" entries.</p>\n");
  }
  body.append("<footer>Served by aeronet</footer>\n</body>\n</html>\n");
  return body;
}

struct DirectoryListingResult {
  vector<DirectoryListingEntry> entries;
  bool truncated{false};
  bool isValid{false};
};

[[nodiscard]] DirectoryListingResult collectDirectoryListing(const std::filesystem::path& directory,
                                                             const StaticFileConfig& config) {
  DirectoryListingResult result;
  const std::size_t limit =
      config.maxEntriesToList == 0U ? std::numeric_limits<std::size_t>::max() : config.maxEntriesToList;

  std::error_code ec;
  std::filesystem::directory_iterator iter(directory, ec);
  if (ec) {
    log::error("Failed to open directory for listing '{}': {}", directory.c_str(), ec.message());
    return result;
  }

  const std::filesystem::directory_iterator end;

  while (!ec && iter != end) {
    const std::filesystem::directory_entry current = *iter;
    iter.increment(ec);
    if (ec) {
      log::warn("Failed to advance directory iterator for '{}': {}", directory.c_str(), ec.message());
    }
    const bool hasMore = !ec && iter != end;

    std::string name = current.path().filename().string();
    if (!config.showHiddenFiles && isHiddenName(name)) {
      continue;
    }

    DirectoryListingEntry& info = result.entries.emplace_back();
    info.name = std::move(name);
    info.entry = current;

    std::error_code stepEc;
    const auto entryStatus = current.symlink_status(stepEc);
    if (stepEc) {
      log::error("Failed to get status for directory entry '{}': {}", current.path().c_str(), stepEc.message());
    } else {
      if (std::filesystem::is_directory(entryStatus)) {
        info.isDirectory = true;
      } else {
        const auto fileSize = current.file_size(stepEc);
        if (stepEc) {
          log::error("Failed to get size for directory entry '{}': {}", current.path().c_str(), stepEc.message());
        } else {
          info.sizeKnown = true;
          info.sizeBytes = fileSize;
        }
      }
    }

    const auto writeTime = current.last_write_time(stepEc);
    if (stepEc) {
      log::error("Failed to get last write time for directory entry '{}': {}", current.path().c_str(),
                 stepEc.message());
    } else {
      info.lastModified = std::chrono::clock_cast<SysClock>(writeTime);
    }

    if (limit != std::numeric_limits<std::size_t>::max() && result.entries.size() >= limit) {
      result.truncated = hasMore;
      break;
    }
  }

  if (ec) {
    return result;
  }

  result.isValid = true;

  std::ranges::sort(result.entries, [](const DirectoryListingEntry& lhs, const DirectoryListingEntry& rhs) {
    // Directories first
    if (lhs.isDirectory != rhs.isDirectory) {
      return lhs.isDirectory && !rhs.isDirectory;
    }
    return lhs.name < rhs.name;
  });
  return result;
}

[[nodiscard]] std::string_view trim(std::string_view value) {
  auto beg = value.begin();
  auto end = value.end();
  while (beg != end && isspace(*beg)) {
    ++beg;
  }
  while (beg != end && isspace(*(end - 1))) {
    --end;
  }
  return {beg, end};
}

// Helper to append a Last-Modified header using a transient stack buffer. The HttpResponse copies
// the header value synchronously, so using a local char buffer is safe and avoids an extra std::string.
inline void addLastModifiedHeader(HttpResponse& resp, SysTimePoint tp) {
  std::array<char, kRFC7231DateStrLen> buf;
  auto* end = TimeToStringRFC7231(tp, buf.data());
  const std::size_t len = static_cast<std::size_t>(end - buf.data());
  assert(len == kRFC7231DateStrLen);
  resp.addHeader(http::LastModified, std::string_view(buf.data(), len));
}

inline constexpr std::size_t kMaxHexChars = sizeof(std::uint64_t) * 2;
inline constexpr std::size_t kMaxEtagSize = 1 + kMaxHexChars + 1 + kMaxHexChars + 1;

struct EtagBuf {
  std::array<char, kMaxEtagSize> buf;
  std::uint8_t len{0};
};

[[nodiscard]] EtagBuf makeStrongEtag(std::uint64_t fileSize, SysTimePoint lastModified) {
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(lastModified.time_since_epoch()).count();

  EtagBuf etag;

  static constexpr char kHexits[] = "0123456789abcdef";

  // start with opening quote
  etag.buf[etag.len++] = '"';

  // helper to write minimal hex digits of 'value' directly into etag.buf at current len
  auto write_hex_direct = [&etag](std::uint64_t value) {
    if (value == 0) {
      etag.buf[etag.len++] = '0';
      return;
    }
    // count hex digits
    std::uint8_t digits = 0;
    std::uint64_t tmp = value;
    while (tmp != 0) {
      ++digits;
      tmp >>= 4U;
    }
    // write from most-significant nibble to least
    for (std::int32_t pos = static_cast<std::int32_t>(digits) - 1; pos >= 0; --pos) {
      const auto shift = static_cast<std::uint64_t>(pos) * 4U;
      const unsigned int nibble = static_cast<unsigned int>((value >> shift) & 0xFU);
      etag.buf[etag.len++] = kHexits[nibble];
    }
  };

  // file size
  write_hex_direct(fileSize);
  // hyphen
  etag.buf[etag.len++] = '-';
  // nanoseconds timestamp
  write_hex_direct(static_cast<std::uint64_t>(nanos));
  // closing quote
  etag.buf[etag.len++] = '"';
  return etag;
}

struct RangeSelection {
  enum class State : std::uint8_t { None, Valid, Invalid, Unsatisfiable };

  State state{State::None};
  std::uint64_t offset{0};
  std::uint64_t length{0};
};

inline constexpr std::uint64_t kInvalidUint64 = std::numeric_limits<std::uint64_t>::max();

[[nodiscard]] std::uint64_t parseUint(std::string_view token) {
  token = trim(token);
  if (token.empty()) {
    return kInvalidUint64;
  }
  std::uint64_t value;
  const auto first = token.data();
  const auto last = token.data() + token.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last) {
    return kInvalidUint64;
  }
  return value;
}

// Note: only single-range requests are supported here (e.g. "Range: bytes=N-M").
// Multi-range requests (comma-separated ranges) and multipart/byteranges
// responses are intentionally not implemented. Supporting multiple ranges requires
// generating multipart/byteranges bodies with unique boundaries and careful
// streaming/memory handling; implement only if you need RFC7233 multipart support.
//
// Also note: ETag comparisons in this file use strong validators. Weak ETags
// (prefixed with "W/") are treated as non-matching for strong comparisons. We
// derive ETags from file size and modification time and require exact (strong)
// matches for conditional semantics (304/412 decisions).
[[nodiscard]] RangeSelection parseRange(std::string_view raw, std::uint64_t fileSize) {
  RangeSelection result;
  if (raw.empty()) {
    return result;
  }
  raw = trim(raw);
  static constexpr std::string_view kBytesEqual = "bytes=";
  if (!StartsWithCaseInsensitive(raw, kBytesEqual)) {
    result.state = RangeSelection::State::Invalid;
    return result;
  }
  raw.remove_prefix(kBytesEqual.size());
  raw = trim(raw);
  if (raw.empty()) {
    result.state = RangeSelection::State::Invalid;
    return result;
  }
  if (raw.contains(',')) {
    result.state = RangeSelection::State::Invalid;
    return result;
  }
  const auto dashPos = raw.find('-');
  if (dashPos == std::string_view::npos) {
    result.state = RangeSelection::State::Invalid;
    return result;
  }
  auto firstPart = trim(raw.substr(0, dashPos));
  auto secondPart = trim(raw.substr(dashPos + 1));

  if (fileSize == 0) {
    result.state = RangeSelection::State::Unsatisfiable;
    return result;
  }

  if (firstPart.empty()) {
    // suffix-byte-range-spec: bytes=-N (last N bytes)
    auto suffixLen = parseUint(secondPart);
    if (suffixLen == kInvalidUint64 || suffixLen == 0) {
      result.state = RangeSelection::State::Invalid;
      return result;
    }
    const std::uint64_t len = std::min<std::uint64_t>(suffixLen, fileSize);
    result.offset = fileSize - len;
    result.length = len;
    result.state = RangeSelection::State::Valid;
    return result;
  }

  auto firstValue = parseUint(firstPart);
  if (firstValue == kInvalidUint64) {
    result.state = RangeSelection::State::Invalid;
    return result;
  }
  if (firstValue >= fileSize) {
    result.state = RangeSelection::State::Unsatisfiable;
    return result;
  }

  if (secondPart.empty()) {
    result.offset = firstValue;
    result.length = fileSize - firstValue;
    result.state = RangeSelection::State::Valid;
    return result;
  }

  auto secondValue = parseUint(secondPart);
  if (secondValue == kInvalidUint64) {
    result.state = RangeSelection::State::Invalid;
    return result;
  }
  if (secondValue < firstValue) {
    result.state = RangeSelection::State::Unsatisfiable;
    return result;
  }
  const std::uint64_t endInclusive = std::min<std::uint64_t>(secondValue, fileSize - 1);
  result.offset = firstValue;
  result.length = endInclusive - firstValue + 1;
  result.state = RangeSelection::State::Valid;
  return result;
}

[[nodiscard]] bool etagTokenMatches(std::string_view token, std::string_view etag) {
  token = trim(token);
  if (token.empty()) {
    return false;
  }
  // "*" matches any current entity tag (RFC 7232 §3.2)
  if (token == "*") {
    return true;
  }
  if (token.size() > 2 && token.starts_with("W/")) {
    return false;  // weak comparison fails for strong validator
  }
  return token == etag;
}

[[nodiscard]] bool etagListMatches(std::string_view headerValue, std::string_view etag) {
  headerValue = trim(headerValue);
  if (headerValue == "*") {
    return true;
  }
  while (!headerValue.empty()) {
    const auto commaPos = headerValue.find(',');
    const auto token = commaPos == std::string_view::npos ? headerValue : headerValue.substr(0, commaPos);
    if (etagTokenMatches(token, etag)) {
      return true;
    }
    if (commaPos == std::string_view::npos) {
      break;
    }
    headerValue.remove_prefix(commaPos + 1);
  }
  return false;
}

struct ConditionalOutcome {
  enum class Kind : std::uint8_t { None, NotModified, PreconditionFailed };

  Kind kind{Kind::None};
  bool rangeAllowed{true};
  http::StatusCode status{http::StatusCodeOK};
};

[[nodiscard]] ConditionalOutcome evaluateConditionals(const HttpRequest& request, bool isGetOrHead,
                                                      std::string_view etag, SysTimePoint lastModified) {
  ConditionalOutcome outcome;

  if (etag.empty() && lastModified == kInvalidTimePoint) {
    return outcome;
  }

  if (auto ifMatch = request.headerValue(http::IfMatch); ifMatch.has_value()) {
    if (etag.empty() || !etagListMatches(*ifMatch, etag)) {
      outcome.kind = ConditionalOutcome::Kind::PreconditionFailed;
      outcome.status = http::StatusCodePreconditionFailed;
      outcome.rangeAllowed = false;
      return outcome;
    }
  }

  if (auto ifUnmodified = request.headerValue(http::IfUnmodifiedSince); ifUnmodified.has_value()) {
    if (lastModified == kInvalidTimePoint) {
      return outcome;
    }
    const auto parsed = TryParseTimeRFC7231(*ifUnmodified);
    if (parsed == kInvalidTimePoint) {
      return outcome;
    }
    if (lastModified > parsed) {
      outcome.kind = ConditionalOutcome::Kind::PreconditionFailed;
      outcome.status = http::StatusCodePreconditionFailed;
      outcome.rangeAllowed = false;
      return outcome;
    }
  }

  if (auto ifNoneMatch = request.headerValue(http::IfNoneMatch); ifNoneMatch.has_value()) {
    if (etagListMatches(*ifNoneMatch, etag)) {
      outcome.rangeAllowed = false;
      outcome.kind = isGetOrHead ? ConditionalOutcome::Kind::NotModified : ConditionalOutcome::Kind::PreconditionFailed;
      outcome.status = isGetOrHead ? http::StatusCodeNotModified : http::StatusCodePreconditionFailed;
    }
    return outcome;
  }

  if (auto ifModified = request.headerValue(http::IfModifiedSince); ifModified.has_value() && isGetOrHead) {
    if (lastModified == kInvalidTimePoint) {
      return outcome;
    }
    const auto parsed = TryParseTimeRFC7231(*ifModified);
    if (parsed == kInvalidTimePoint) {
      return outcome;
    }
    if (lastModified <= parsed) {
      outcome.kind = ConditionalOutcome::Kind::NotModified;
      outcome.status = http::StatusCodeNotModified;
      outcome.rangeAllowed = false;
      return outcome;
    }
  }

  return outcome;
}

[[nodiscard]] bool ifRangeAllowsPartial(std::string_view value, std::string_view etag, SysTimePoint lastModified) {
  value = trim(value);
  if (value.empty()) {
    return false;
  }
  if (value.front() == '"') {
    return value == etag;
  }
  if (value.starts_with("W/")) {
    return false;
  }
  const auto parsed = TryParseTimeRFC7231(value);
  if (parsed == kInvalidTimePoint || lastModified == kInvalidTimePoint) {
    return false;
  }
  return lastModified <= parsed;
}

inline constexpr std::string_view kBytesPrefixStr = "bytes ";
inline constexpr std::size_t kMaxRangeHeaderLen =
    kBytesPrefixStr.size() + (3UL * ndigits(std::numeric_limits<std::uint64_t>::max())) + 2UL;  // +2 for '-' and '/'

struct RangeHeaderBuf {
  static_assert(kMaxRangeHeaderLen <= std::numeric_limits<std::uint8_t>::max());

  std::array<char, kMaxRangeHeaderLen> buf;
  std::uint8_t len;
};

[[nodiscard]] RangeHeaderBuf buildRangeHeader(std::uint64_t start, std::uint64_t length, std::uint64_t total) {
  RangeHeaderBuf result;

  std::memcpy(result.buf.data(), kBytesPrefixStr.data(), kBytesPrefixStr.size());
  result.len = static_cast<std::uint8_t>(kBytesPrefixStr.size());

  {
    const auto startBuf = IntegralToCharVector(start);
    std::memcpy(result.buf.data() + result.len, startBuf.data(), startBuf.size());
    result.len += static_cast<std::uint8_t>(startBuf.size());
    result.buf[result.len] = '-';
    ++result.len;
  }

  {
    const auto endBuf = IntegralToCharVector(start + length - 1);
    std::memcpy(result.buf.data() + result.len, endBuf.data(), endBuf.size());
    result.len += static_cast<std::uint8_t>(endBuf.size());
    result.buf[result.len] = '/';
    ++result.len;
  }

  {
    const auto totalBuf = IntegralToCharVector(total);
    std::memcpy(result.buf.data() + result.len, totalBuf.data(), totalBuf.size());
    result.len += static_cast<std::uint8_t>(totalBuf.size());
  }
  return result;
}

inline constexpr std::string_view kUnsatisfiedRangePrefixStr = "bytes */";
inline constexpr std::size_t kMaxUnsatisfiedRangeHeaderLen =
    kUnsatisfiedRangePrefixStr.size() + ndigits(std::numeric_limits<std::uint64_t>::max());

struct UnsatisfiedRangeHeaderBuf {
  static_assert(kMaxUnsatisfiedRangeHeaderLen <= std::numeric_limits<std::uint8_t>::max());

  std::array<char, kMaxUnsatisfiedRangeHeaderLen> buf;
  std::uint8_t len;
};

[[nodiscard]] UnsatisfiedRangeHeaderBuf buildUnsatisfiedRangeHeader(std::uint64_t total) {
  UnsatisfiedRangeHeaderBuf result;
  std::memcpy(result.buf.data(), kUnsatisfiedRangePrefixStr.data(), kUnsatisfiedRangePrefixStr.size());
  result.len = static_cast<std::uint8_t>(kUnsatisfiedRangePrefixStr.size());

  const auto totalBuf = IntegralToCharVector(total);
  std::memcpy(result.buf.data() + result.len, totalBuf.data(), totalBuf.size());
  result.len += static_cast<std::uint8_t>(totalBuf.size());

  return result;
}

}  // namespace

StaticFileHandler::StaticFileHandler(std::filesystem::path rootDirectory, StaticFileConfig config)
    : _root(std::move(rootDirectory)), _config(std::move(config)) {
  _config.validate();

  std::error_code ec;
  _root = std::filesystem::weakly_canonical(_root, ec);
  if (ec) {
    _root = std::filesystem::absolute(_root);
  }
  if (!std::filesystem::exists(_root) || !std::filesystem::is_directory(_root)) {
    throw std::invalid_argument("StaticFileHandler root must be an existing directory");
  }
}

bool StaticFileHandler::resolveTarget(const HttpRequest& request, std::filesystem::path& resolvedPath) const {
  std::string_view rawPath = request.path();
  if (rawPath.empty()) {
    rawPath = "/";
  }
  const bool requestedTrailingSlash = rawPath.ends_with('/');
  if (rawPath.front() == '/') {
    rawPath.remove_prefix(1);
  }
  std::filesystem::path relative;
  while (!rawPath.empty()) {
    const auto slashPos = rawPath.find('/');
    const auto segment = rawPath.substr(0, slashPos);
    if (!segment.empty() && segment != ".") {
      if (segment == "..") {
        return false;
      }
      relative /= std::filesystem::path(segment);
    }
    if (slashPos == std::string_view::npos) {
      break;
    }
    rawPath.remove_prefix(slashPos + 1);
  }

  resolvedPath = _root / relative;
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(resolvedPath, ec);
  if (ec) {
    return false;
  }
  if (std::filesystem::is_directory(status)) {
    if (!_config.defaultIndex().empty()) {
      std::filesystem::path indexPath = resolvedPath / _config.defaultIndex();
      std::error_code indexEc;
      const auto indexStatus = std::filesystem::symlink_status(indexPath, indexEc);
      if (!indexEc && std::filesystem::is_regular_file(indexStatus)) {
        resolvedPath = std::move(indexPath);
        return true;
      }
    }
    return _config.enableDirectoryIndex;
  }

  return !requestedTrailingSlash;
}

namespace {
HttpResponse MakeError(http::StatusCode code, std::string_view reason) {
  HttpResponse resp(code, reason);

  RawChars body(reason.size() + 1UL);
  body.unchecked_append(reason);
  body.unchecked_push_back('\n');

  resp.body(std::move(body));
  return resp;
}
}  // namespace

HttpResponse StaticFileHandler::operator()(const HttpRequest& request) const {
  const bool isGet = request.method() == http::Method::GET;
  const bool isHead = request.method() == http::Method::HEAD;

  if (!isGet && !isHead) {
    HttpResponse resp(http::StatusCodeMethodNotAllowed, http::ReasonMethodNotAllowed);
    resp.addHeader(http::Allow, "GET, HEAD");
    resp.body("Method Not Allowed\n");
    return resp;
  }

  std::string_view requestPath = request.path();
  if (requestPath.empty()) {
    requestPath = "/";
  }

  const bool requestedTrailingSlash = requestPath.ends_with('/');

  std::filesystem::path targetPath;
  if (!resolveTarget(request, targetPath)) {
    return MakeError(http::StatusCodeNotFound, http::NotFound);
  }

  std::error_code ec;
  const auto status = std::filesystem::symlink_status(targetPath, ec);
  if (ec) {
    return MakeError(http::StatusCodeNotFound, http::NotFound);
  }

  if (std::filesystem::is_directory(status)) {
    if (!_config.enableDirectoryIndex) {
      return MakeError(http::StatusCodeNotFound, http::NotFound);
    }

    if (!requestedTrailingSlash) {
      std::string location(requestPath);
      if (!location.empty() && !location.ends_with('/')) {
        location.push_back('/');
      }
      HttpResponse resp(http::StatusCodeMovedPermanently, http::MovedPermanently);
      resp.location(location);
      resp.addHeader("Cache-Control", "no-cache");
      resp.body("Moved Permanently\n");
      return resp;
    }

    auto listing = collectDirectoryListing(targetPath, _config);
    if (!listing.isValid) {
      return MakeError(http::StatusCodeInternalServerError, http::ReasonInternalServerError);
    }

    HttpResponse resp(http::StatusCodeOK, http::ReasonOK);
    resp.addHeader("Cache-Control", "no-cache");
    resp.addHeader("X-Directory-Listing-Truncated", listing.truncated ? "1" : "0");

    static constexpr std::string_view kContentType = "text/html; charset=utf-8";
    if (_config.directoryIndexRenderer) {
      vector<std::filesystem::directory_entry> rawEntries(listing.entries.size());
      std::ranges::transform(listing.entries, rawEntries.begin(),
                             [](const DirectoryListingEntry& entry) { return entry.entry; });
      resp.body(_config.directoryIndexRenderer(targetPath, rawEntries), kContentType);
    } else {
      resp.body(
          renderDefaultDirectoryListing(requestPath, listing.entries, listing.truncated, _config.directoryListingCss()),
          kContentType);
    }
    return resp;
  }

  if (!std::filesystem::exists(status) || !std::filesystem::is_regular_file(status)) {
    return MakeError(http::StatusCodeNotFound, http::NotFound);
  }

  const auto fileSize = std::filesystem::file_size(targetPath, ec);
  if (ec) {
    return MakeError(http::StatusCodeNotFound, http::NotFound);
  }

  SysTimePoint lastModified = kInvalidTimePoint;
  if ((_config.addLastModified || _config.enableConditional) && !ec) {
    const auto writeTime = std::filesystem::last_write_time(targetPath, ec);
    if (!ec) {
      lastModified = std::chrono::clock_cast<SysClock>(writeTime);
    }
  }

  File file(targetPath.string(), File::OpenMode::ReadOnly);
  if (!file) {
    return MakeError(http::StatusCodeNotFound, http::NotFound);
  }

  EtagBuf etag;
  if ((_config.addEtag || _config.enableConditional) && lastModified != kInvalidTimePoint) {
    etag = makeStrongEtag(fileSize, lastModified);
  }

  std::string_view etagView{etag.buf.data(), etag.len};

  const bool isConditional = _config.enableConditional;
  ConditionalOutcome conditionalOutcome;
  if (isConditional) {
    conditionalOutcome = evaluateConditionals(request, isGet || isHead, etagView, lastModified);
    if (conditionalOutcome.kind == ConditionalOutcome::Kind::PreconditionFailed) {
      HttpResponse resp(conditionalOutcome.status, "Precondition Failed");
      if (!etagView.empty()) {
        resp.addHeader(http::ETag, etagView);
      }
      if (_config.addLastModified && lastModified != kInvalidTimePoint) {
        addLastModifiedHeader(resp, lastModified);
      }
      resp.addHeader(http::AcceptRanges, "bytes");
      resp.body("Precondition Failed\n");
      return resp;
    }
    if (conditionalOutcome.kind == ConditionalOutcome::Kind::NotModified) {
      HttpResponse resp(http::StatusCodeNotModified, "Not Modified");
      if (!etagView.empty()) {
        resp.addHeader(http::ETag, etagView);
      }
      if (_config.addLastModified && lastModified != kInvalidTimePoint) {
        addLastModifiedHeader(resp, lastModified);
      }
      resp.addHeader(http::AcceptRanges, "bytes");
      return resp;
    }
  }

  const bool allowRanges = _config.enableRange && conditionalOutcome.rangeAllowed;
  RangeSelection rangeSelection;
  if (allowRanges) {
    if (auto rangeHeader = request.headerValue(http::Range); rangeHeader.has_value()) {
      bool allowed = true;
      if (auto ifRange = request.headerValue(http::IfRange); ifRange.has_value()) {
        allowed = ifRangeAllowsPartial(*ifRange, etagView, lastModified);
      }
      if (allowed) {
        rangeSelection = parseRange(*rangeHeader, fileSize);
      }
    }
  }

  if (rangeSelection.state == RangeSelection::State::Invalid) {
    HttpResponse resp(http::StatusCodeRangeNotSatisfiable, "Range Not Satisfiable");
    const auto rangeHeader = buildUnsatisfiedRangeHeader(fileSize);
    resp.addHeader(http::ContentRange, std::string_view(rangeHeader.buf.data(), rangeHeader.len));
    resp.addHeader(http::AcceptRanges, "bytes");
    resp.body("Invalid Range\n");
    return resp;
  }

  if (rangeSelection.state == RangeSelection::State::Unsatisfiable) {
    HttpResponse resp(http::StatusCodeRangeNotSatisfiable, "Range Not Satisfiable");
    const auto rangeHeader = buildUnsatisfiedRangeHeader(fileSize);
    resp.addHeader(http::ContentRange, std::string_view(rangeHeader.buf.data(), rangeHeader.len));
    resp.addHeader(http::AcceptRanges, "bytes");
    resp.body("Range Not Satisfiable\n");
    return resp;
  }

  HttpResponse resp(http::StatusCodeOK, http::ReasonOK);
  resp.addHeader(http::AcceptRanges, "bytes");
  if (!etagView.empty()) {
    resp.addHeader(http::ETag, etagView);
  }
  if (_config.addLastModified && lastModified != kInvalidTimePoint) {
    addLastModifiedHeader(resp, lastModified);
  }

  bool contentTypeSet = false;
  if (_config.contentTypeResolver) {
    auto resolved = _config.contentTypeResolver(targetPath.generic_string());
    if (!resolved.empty()) {
      resp.header(http::ContentType, std::move(resolved));
      contentTypeSet = true;
    }
  }

  if (!contentTypeSet) {
    if (_config.defaultContentType().empty()) {
      resp.header(http::ContentType, http::ContentTypeApplicationOctetStream);
    } else {
      resp.header(http::ContentType, _config.defaultContentType());
    }
  }

  if (rangeSelection.state == RangeSelection::State::Valid) {
    resp.status(http::StatusCodePartialContent, "Partial Content");
    const auto rangeHeader = buildRangeHeader(rangeSelection.offset, rangeSelection.length, fileSize);
    resp.addHeader(http::ContentRange, std::string_view(rangeHeader.buf.data(), rangeHeader.len));
    resp.file(std::move(file), static_cast<std::size_t>(rangeSelection.offset),
              static_cast<std::size_t>(rangeSelection.length));
    return resp;
  }

  resp.file(std::move(file));
  return resp;
}

}  // namespace aeronet
