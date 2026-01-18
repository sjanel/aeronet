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
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/bytes-string.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/log.hpp"
#include "aeronet/mime-mappings.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/static-file-config.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"
#include "aeronet/url-encode.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {
namespace {

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

void AppendHtmlEscaped(std::string_view requestPath, RawChars& out) {
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

void FormatLastModified(SysTimePoint tp, RawChars& buf) {
  if (tp == kInvalidTimePoint) {
    buf.push_back('-');
  } else {
    buf.ensureAvailableCapacityExponential(kRFC7231DateStrLen);
    TimeToStringRFC7231(tp, buf.data() + buf.size());
    buf.addSize(kRFC7231DateStrLen);
  }
}

void DefaultDirectoryListingCss(std::string_view customCss, RawChars& buf) {
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
  std::filesystem::directory_entry entry;
  bool isDirectory{false};
  bool fileSizeKnown{false};
  std::uintmax_t sizeBytes{0};
  SysTimePoint lastModified{kInvalidTimePoint};
};

RawChars RenderDefaultDirectoryListing(std::string_view requestPath, std::span<const DirectoryListingEntry> entries,
                                       bool truncated, std::string_view customCss) {
  RawChars body(2048U);

  body.unchecked_append("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<title>Index of ");
  AppendHtmlEscaped(requestPath, body);
  body.append("</title>\n<style>");

  DefaultDirectoryListingCss(customCss, body);

  body.append("</style>\n</head>\n<body>\n<h1>Index of ");
  AppendHtmlEscaped(requestPath, body);
  body.append(
      "</h1>\n<table>\n<thead><tr><th>Name</th><th class=\"size\">Size</th><th class=\"modified\">Last "
      "Modified</th></tr></thead>\n<tbody>\n");

  body.append(
      "<tr><td class=\"name\"><a href=\"../\" class=\"dir\">..</a></td><td class=\"size\">-</td><td "
      "class=\"modified\">-</td></tr>\n");

  for (const auto& entry : entries) {
    const bool isDir = entry.isDirectory;
    const auto encodedSz =
        URLEncodedSize(entry.name, [](char ch) { return kUnreservedTable[static_cast<unsigned char>(ch)]; });

    body.append(R"(<tr><td class="name"><a href=")");

    body.ensureAvailableCapacityExponential(encodedSz);
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
    AppendHtmlEscaped(entry.name, body);

    body.append("</a></td><td class=\"size\">");
    if (entry.fileSizeKnown) {
      AddFormattedSize(entry.sizeBytes, body);
    } else {
      body.push_back('-');
    }
    body.append("</td><td class=\"modified\">");
    FormatLastModified(entry.lastModified, body);
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

[[nodiscard]] DirectoryListingResult CollectDirectoryListing(const std::filesystem::path& directory,
                                                             const StaticFileConfig& config) {
  const std::size_t limit = config.maxEntriesToList;

  DirectoryListingResult result;
  std::error_code ec;
  std::filesystem::directory_iterator it(directory, ec);
  if (ec) {
    log::error("Failed to open directory for listing '{}': {}", directory.c_str(), ec.message());
    return result;
  }

  const auto comp = [](const DirectoryListingEntry& lhs, const DirectoryListingEntry& rhs) {
    // Directories first
    if (lhs.isDirectory != rhs.isDirectory) {
      return lhs.isDirectory;
    }
    return lhs.name < rhs.name;
  };

  const std::filesystem::directory_iterator end;

  while (!ec && it != end) {
    std::filesystem::directory_entry current = *it;
    it.increment(ec);
    if (ec) {
      log::error("Failed to advance directory iterator for '{}': {}", directory.c_str(), ec.message());
    }

    std::string name = current.path().filename().string();
    if (!config.showHiddenFiles && name.starts_with('.')) {
      continue;
    }

    DirectoryListingEntry& info = result.entries.emplace_back(std::move(name));

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
          info.fileSizeKnown = true;
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

    info.entry = std::move(current);

    // Order of directory exploration is file system dependent.
    // So we start to keep an up to date ordered list of elements once it reaches the limit so that the list stays
    // consistent across file systems.
    if (result.entries.size() == limit) {
      std::ranges::sort(result.entries, comp);
    } else if (result.entries.size() > limit) {
      // we have a container of limit + 1 elements, with the last one maybe unsorted.
      // We need to find the correct insert place of last element, and rotate the elements to keep the vector sorted.
      auto lb = std::ranges::lower_bound(result.entries.begin(), result.entries.end() - 1, result.entries.back(), comp);
      // We need to rotate +1 (to the right) the range [lb, end)]
      std::rotate(lb, result.entries.end() - 1, result.entries.end());
      result.entries.pop_back();

      result.truncated = true;
    }
  }

  if (ec) {
    return result;
  }

  result.isValid = true;

  if (result.entries.size() < limit) {
    // If it is at the limit, it's already sorted.
    std::ranges::sort(result.entries, comp);
  }

  return result;
}

// Helper to append a Last-Modified header using a transient stack buffer. The HttpResponse copies
// the header value synchronously, so using a local char buffer is safe and avoids an extra std::string.
void AddLastModifiedHeader(HttpResponse& resp, SysTimePoint tp) {
  std::array<char, kRFC7231DateStrLen> buf;
  auto* end = TimeToStringRFC7231(tp, buf.data());
  assert(std::cmp_equal(end - buf.data(), kRFC7231DateStrLen));
  resp.headerAddLine(http::LastModified, std::string_view(buf.data(), end));
}

inline constexpr std::size_t kMaxHexChars = sizeof(std::uint64_t) * 2;
inline constexpr std::size_t kMaxEtagSize = 1 + kMaxHexChars + 1 + kMaxHexChars + 1;

struct EtagBuf {
  std::array<char, kMaxEtagSize> buf;
  std::uint8_t len{0};
};

void MakeStrongEtag(std::uint64_t fileSize, SysTimePoint lastModified, EtagBuf& etag) {
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(lastModified.time_since_epoch()).count();

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
}

struct RangeSelection {
  enum class State : std::uint8_t { None, Valid, Invalid, Unsatisfiable };

  State state{State::None};
  std::size_t offset{0};
  std::size_t length{0};
};

inline constexpr std::size_t kInvalidSize = std::numeric_limits<std::size_t>::max();

std::size_t ParseSize(std::string_view token) {
  assert(token == TrimOws(token));
  if (token.empty()) {
    return kInvalidSize;
  }
  std::size_t value;
  const auto first = token.data();
  const auto last = first + token.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last) {
    return kInvalidSize;
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
RangeSelection ParseRange(std::string_view raw, std::size_t fileSize) {
  RangeSelection result;
  raw = TrimOws(raw);
  if (raw.empty()) {
    return result;
  }
  static constexpr std::string_view kBytesEqual = "bytes=";
  if (!StartsWithCaseInsensitive(raw, kBytesEqual)) {
    result.state = RangeSelection::State::Invalid;
    return result;
  }
  raw.remove_prefix(kBytesEqual.size());
  raw = TrimOws(raw);
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
  auto firstPart = TrimOws(raw.substr(0, dashPos));
  auto secondPart = TrimOws(raw.substr(dashPos + 1));

  if (fileSize == 0) {
    result.state = RangeSelection::State::Unsatisfiable;
    return result;
  }

  if (firstPart.empty()) {
    // suffix-byte-range-spec: bytes=-N (last N bytes)
    const std::size_t suffixLen = ParseSize(secondPart);
    if (suffixLen == kInvalidSize || suffixLen == 0) {
      result.state = RangeSelection::State::Invalid;
      return result;
    }
    const std::uint64_t len = std::min<std::uint64_t>(suffixLen, fileSize);
    result.offset = fileSize - len;
    result.length = len;
    result.state = RangeSelection::State::Valid;
    return result;
  }

  auto firstValue = ParseSize(firstPart);
  if (firstValue == kInvalidSize) {
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

  auto secondValue = ParseSize(secondPart);
  if (secondValue == kInvalidSize) {
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

bool EtagTokenMatches(std::string_view token, std::string_view etag) {
  token = TrimOws(token);
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

bool EtagListMatches(std::string_view headerValue, std::string_view etag) {
  headerValue = TrimOws(headerValue);
  if (headerValue == "*") {
    return true;
  }
  while (!headerValue.empty()) {
    const auto commaPos = headerValue.find(',');
    const auto token = commaPos == std::string_view::npos ? headerValue : headerValue.substr(0, commaPos);
    if (EtagTokenMatches(token, etag)) {
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

ConditionalOutcome EvaluateConditionals(const HttpRequest& request, std::string_view etag, SysTimePoint lastModified) {
  ConditionalOutcome outcome;

  if (etag.empty()) {
    return outcome;
  }

  if (auto ifMatch = request.headerValue(http::IfMatch); ifMatch.has_value()) {
    if (!EtagListMatches(*ifMatch, etag)) {
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
    if (EtagListMatches(*ifNoneMatch, etag)) {
      outcome.rangeAllowed = false;
      outcome.kind = ConditionalOutcome::Kind::NotModified;
      outcome.status = http::StatusCodeNotModified;
    }
    return outcome;
  }

  if (auto ifModified = request.headerValue(http::IfModifiedSince); ifModified.has_value()) {
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

bool IfRangeAllowsPartial(std::string_view value, std::string_view etag, SysTimePoint lastModified) {
  value = TrimOws(value);
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

RangeHeaderBuf BuildRangeHeader(std::uint64_t start, std::uint64_t length, std::uint64_t total) {
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

UnsatisfiedRangeHeaderBuf BuildUnsatisfiedRangeHeader(std::uint64_t total) {
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
  const bool requestedTrailingSlash = rawPath.back() == '/';

  rawPath.remove_prefix(1);

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

HttpResponse StaticFileHandler::operator()(const HttpRequest& request) const {
  HttpResponse resp = request.makeResponse(32UL, http::StatusCodeNotFound);

  if (request.method() != http::Method::GET && request.method() != http::Method::HEAD) {
    resp.status(http::StatusCodeMethodNotAllowed);
    resp.headerAddLine(http::Allow, "GET, HEAD");
    return resp;
  }

  const std::string_view requestPath = request.path();
  const bool requestedTrailingSlash = requestPath.back() == '/';

  std::filesystem::path targetPath;
  if (!resolveTarget(request, targetPath)) {
    return resp;
  }

  std::error_code ec;
  const auto status = std::filesystem::symlink_status(targetPath, ec);
  if (ec) {
    return resp;
  }

  if (std::filesystem::is_directory(status)) {
    if (!_config.enableDirectoryIndex) {
      return resp;
    }

    if (!requestedTrailingSlash) {
      const std::size_t appendSlash = requestedTrailingSlash ? 0UL : 1UL;
      RawChars location(requestPath.size() + appendSlash);
      location.unchecked_append(requestPath);
      if (appendSlash != 0) {
        location.unchecked_push_back('/');
      }
      resp.status(http::StatusCodeMovedPermanently);
      resp.location(location);
      resp.headerAddLine(http::CacheControl, "no-cache");
      resp.body("Moved Permanently\n");
      return resp;
    }

    auto listing = CollectDirectoryListing(targetPath, _config);
    if (!listing.isValid) {
      resp.status(http::StatusCodeInternalServerError);
      return resp;
    }

    resp.status(http::StatusCodeOK);
    resp.headerAddLine(http::CacheControl, "no-cache");
    resp.headerAddLine(http::XDirectoryListingTruncated, listing.truncated ? "1" : "0");

    static constexpr std::string_view kContentType = "text/html; charset=utf-8";
    if (_config.directoryIndexRenderer) {
      vector<std::filesystem::directory_entry> rawEntries(listing.entries.size());
      std::transform(std::make_move_iterator(listing.entries.begin()), std::make_move_iterator(listing.entries.end()),
                     rawEntries.begin(), [](DirectoryListingEntry&& entry) { return std::move(entry.entry); });
      resp.body(_config.directoryIndexRenderer(targetPath, rawEntries), kContentType);
    } else {
      resp.body(
          RenderDefaultDirectoryListing(requestPath, listing.entries, listing.truncated, _config.directoryListingCss()),
          kContentType);
    }
    return resp;
  }

  std::string_view targetPathString(targetPath.c_str());

  File file(targetPath.c_str(), File::OpenMode::ReadOnly);
  if (!file) {
    resp.body("Unable to open file\n");
    return resp;
  }

  // TODO: is it possible to make only one system call to open file and get its size/metadata?
  const std::size_t fileSize = file.size();
  if (fileSize == File::kError) {
    resp.body("Unable to read file size\n");
    return resp;
  }

  SysTimePoint lastModified = kInvalidTimePoint;
  if (_config.addLastModified || _config.enableConditional || _config.addEtag) {
    const auto writeTime = std::filesystem::last_write_time(targetPath, ec);
    if (!ec) {
      lastModified = std::chrono::clock_cast<SysClock>(writeTime);
    }
  }

  EtagBuf etag;
  if ((_config.addEtag || _config.enableConditional) && lastModified != kInvalidTimePoint) {
    MakeStrongEtag(fileSize, lastModified, etag);
  }

  std::string_view etagView{etag.buf.data(), etag.len};

  resp.headerAddLine(http::AcceptRanges, "bytes");
  if (_config.addEtag && !etagView.empty()) {
    resp.headerAddLine(http::ETag, etagView);
  }
  if (_config.addLastModified && lastModified != kInvalidTimePoint) {
    AddLastModifiedHeader(resp, lastModified);
  }

  ConditionalOutcome conditionalOutcome;
  if (_config.enableConditional) {
    conditionalOutcome = EvaluateConditionals(request, etagView, lastModified);
    if (conditionalOutcome.kind == ConditionalOutcome::Kind::PreconditionFailed) {
      resp.status(conditionalOutcome.status);
      resp.body("Precondition Failed\n");
      return resp;
    }
    if (conditionalOutcome.kind == ConditionalOutcome::Kind::NotModified) {
      resp.status(http::StatusCodeNotModified);
      return resp;
    }
  }

  // Resolve content type.
  // Algorithm is:
  // 1) If user provided a content type resolver, use that (it can still return empty to indicate no preference).
  // 2) If that returned empty (or no content type resolver), try to determine MIME type from file extension.
  // 3) If that also failed, use default content type from config.
  std::string_view contentTypeForFile;
  if (_config.contentTypeResolver) {
    contentTypeForFile = _config.contentTypeResolver(targetPathString);
  }

  if (contentTypeForFile.empty()) {
    contentTypeForFile = DetermineMIMETypeStr(targetPathString);
    if (contentTypeForFile.empty()) {
      contentTypeForFile = _config.defaultContentType();
    }
  }

  const bool allowRanges = _config.enableRange && conditionalOutcome.rangeAllowed;
  if (allowRanges) {
    if (auto rangeHeader = request.headerValue(http::Range); rangeHeader.has_value()) {
      bool allowed = true;
      if (auto ifRange = request.headerValue(http::IfRange); ifRange.has_value()) {
        allowed = IfRangeAllowsPartial(*ifRange, etagView, lastModified);
      }
      if (allowed) {
        RangeSelection rangeSelection = ParseRange(*rangeHeader, fileSize);

        if (rangeSelection.state == RangeSelection::State::Invalid ||
            rangeSelection.state == RangeSelection::State::Unsatisfiable) {
          const auto rangeHeader = BuildUnsatisfiedRangeHeader(fileSize);
          resp.status(http::StatusCodeRangeNotSatisfiable);
          resp.headerAddLine(http::ContentRange, std::string_view(rangeHeader.buf.data(), rangeHeader.len));
          if (rangeSelection.state == RangeSelection::State::Invalid) {
            resp.body("Invalid Range\n");
          } else {
            resp.body("Range Not Satisfiable\n");
          }
          return resp;
        }
        if (rangeSelection.state == RangeSelection::State::Valid) {
          const auto rangeHeader = BuildRangeHeader(rangeSelection.offset, rangeSelection.length, fileSize);

          resp.status(http::StatusCodePartialContent);
          resp.headerAddLine(http::ContentRange, std::string_view(rangeHeader.buf.data(), rangeHeader.len));
          resp.file(std::move(file), rangeSelection.offset, rangeSelection.length, contentTypeForFile);
          return resp;
        }
      }
    }
  }

  resp.status(http::StatusCodeOK);
  resp.file(std::move(file), contentTypeForFile);
  return resp;
}

}  // namespace aeronet
