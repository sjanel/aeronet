#include "aeronet/static-file-handler.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/bytes-string.hpp"
#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/chrono-lite.hpp"
#include "aeronet/decimal-writer.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/http-response-access.hpp"
#include "aeronet/log.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/mime-mappings.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/static-file-config.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"
#include "aeronet/url-char-class.hpp"
#include "aeronet/url-encode.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

namespace http {

namespace {
constexpr LowerAsciiKey IfRange = "if-range";
constexpr LowerAsciiKey IfUnmodifiedSince = "if-unmodified-since";
constexpr LowerAsciiKey IfMatch = "if-match";
}  // namespace

}  // namespace http

namespace {

struct PathString {
  // On POSIX: just a view into the path's native buffer (no allocation)
  // On Windows: owns the converted string, exposes a view into it
#ifdef AERONET_WINDOWS
  explicit PathString(const std::filesystem::path& p) : owned(p.string()), view(owned) {}

  [[nodiscard]] const char* c_str() const { return owned.c_str(); }

  std::string owned;
#else
  explicit PathString(const std::filesystem::path& path) : p(path), view(path.native()) {}

  [[nodiscard]] const char* c_str() const { return p.c_str(); }

  const std::filesystem::path& p;
#endif
  std::string_view view;
};

// Dotfiles and dot-directories are hidden, except '.well-known' that RFC 8615 reserves for public metadata.
// Templated on the character type of native paths (wchar_t on Windows).
template <class CharT>
constexpr bool IsHiddenName(std::basic_string_view<CharT> name) {
  static constexpr std::string_view kWellKnown = ".well-known";
  return name.starts_with(CharT{'.'}) && !std::ranges::equal(name, kWellKnown);
}

// Tell whether a segment of 'relativePath' (native separators) is a hidden name.
template <class CharT>
bool HasHiddenSegment(std::basic_string_view<CharT> relativePath) {
  static constexpr auto kSep = std::filesystem::path::preferred_separator;
  while (!relativePath.empty()) {
    const auto sepPos = relativePath.find(kSep);
    if (IsHiddenName(relativePath.substr(0, sepPos))) {
      return true;
    }
    if (sepPos == std::basic_string_view<CharT>::npos) {
      break;
    }
    relativePath.remove_prefix(sepPos + 1);
  }
  return false;
}

// The HTML escape sequence of a character, empty when it is written as is.
constexpr std::string_view HtmlEscapeSequence(char ch) {
  switch (ch) {
    case '&':
      return "&amp;";
    case '<':
      return "&lt;";
    case '>':
      return "&gt;";
    case '"':
      return "&quot;";
    case '\'':
      return "&#39;";
    default:
      return {};
  }
}

// The number of chars that WriteHtmlEscaped(str, out) writes.
std::size_t HtmlEscapedSize(std::string_view str) {
  std::size_t size = str.size();
  for (char ch : str) {
    const std::string_view escapeSequence = HtmlEscapeSequence(ch);
    if (!escapeSequence.empty()) {
      size += escapeSequence.size() - 1U;
    }
  }
  return size;
}

char* WriteHtmlEscaped(std::string_view str, char* pData) {
  for (char ch : str) {
    const std::string_view escapeSequence = HtmlEscapeSequence(ch);
    if (escapeSequence.empty()) {
      *pData++ = ch;
    } else {
      pData = Append(escapeSequence, pData);
    }
  }
  return pData;
}

// The parts of the default directory listing around the request path, the CSS and the entries.
constexpr std::string_view kListingHead =
    "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<title>Index of ";
constexpr std::string_view kListingTitleEnd = "</title>\n<style>";
constexpr std::string_view kListingStyleEnd = "</style>\n</head>\n<body>\n<h1>Index of ";
constexpr std::string_view kListingTableStart =
    "</h1>\n<table>\n<thead><tr><th>Name</th><th class=\"size\">Size</th><th class=\"modified\">Last "
    "Modified</th></tr></thead>\n<tbody>\n"
    "<tr><td class=\"name\"><a href=\"../\" class=\"dir\">..</a></td><td class=\"size\">-</td><td "
    "class=\"modified\">-</td></tr>\n";
constexpr std::string_view kListingTableEnd = "</tbody>\n</table>\n";
constexpr std::string_view kListingTruncatedStart = "<p id=\"truncated\">Listing truncated after ";
constexpr std::string_view kListingTruncatedEnd = " entries.</p>\n";
constexpr std::string_view kListingFooter = "<footer>Served by aeronet</footer>\n</body>\n</html>\n";

constexpr std::string_view kDefaultDirectoryListingCss = R"CSS(
body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:2rem;}
table{border-collapse:collapse;width:100%;max-width:960px;}
th,td{padding:0.3rem 0.6rem;text-align:left;border-bottom:1px solid #e0e0e0;}
tbody tr:hover{background:#f8f8f8;}
td.size,td.modified{text-align:right;font-variant-numeric:tabular-nums;}
h1{font-size:1.4rem;margin-bottom:1rem;}
#truncated{margin-top:1rem;color:#b24e00;}
footer{margin-top:2rem;font-size:0.85rem;color:#666;}
a.dir::after{content:"/";}
)CSS";

// The markup of the row of an entry around its name, its size and its modification time.
constexpr std::string_view kEntryRowStart = R"(<tr><td class="name"><a href=")";
constexpr std::string_view kEntryDirClass = R"( class="dir")";
constexpr std::string_view kEntrySizeStart = R"(</a></td><td class="size">)";
constexpr std::string_view kEntryModifiedStart = R"(</td><td class="modified">)";
constexpr std::string_view kEntryRowEnd = "</td></tr>\n";

struct DirectoryListingEntry {
  std::string name;
  std::filesystem::directory_entry entry;
  bool isDirectory{false};
  bool fileSizeKnown{false};
  std::uintmax_t sizeBytes{0};
  SysTimePoint lastModified{kInvalidTimePoint};
};

// The number of chars that WriteListingRow(entry, out) writes.
std::size_t ListingRowSize(const DirectoryListingEntry& entry) {
  std::size_t size = kEntryRowStart.size() + URLEncodedSize(entry.name, url::IsUnreserved) + 2U /* '"' and '>' */ +
                     HtmlEscapedSize(entry.name) + kEntrySizeStart.size() + kEntryModifiedStart.size() +
                     kEntryRowEnd.size();
  if (entry.isDirectory) {
    size += 1U /* '/' */ + kEntryDirClass.size();
  }
  size += entry.fileSizeKnown ? FormattedSizeLen(entry.sizeBytes) : 1U;
  size += entry.lastModified == kInvalidTimePoint ? 1U : RFC7231DateStrLen;
  return size;
}

char* WriteListingRow(const DirectoryListingEntry& entry, char* pData) {
  pData = Append(kEntryRowStart, pData);
  pData = URLEncode(entry.name, url::IsUnreserved, pData);
  if (entry.isDirectory) {
    *pData++ = '/';  // the link of a directory ends with '/'
  }
  *pData++ = '"';
  if (entry.isDirectory) {
    pData = Append(kEntryDirClass, pData);
  }
  *pData++ = '>';
  pData = WriteHtmlEscaped(entry.name, pData);
  pData = Append(kEntrySizeStart, pData);
  if (entry.fileSizeKnown) {
    pData = WriteFormattedSize(entry.sizeBytes, pData);
  } else {
    *pData++ = '-';
  }
  pData = Append(kEntryModifiedStart, pData);
  if (entry.lastModified == kInvalidTimePoint) {
    *pData++ = '-';
  } else {
    pData = TimeToStringRFC7231(entry.lastModified, pData);
  }
  return Append(kEntryRowEnd, pData);
}

RawChars RenderDefaultDirectoryListing(std::string_view requestPath, std::span<const DirectoryListingEntry> entries,
                                       bool truncated, std::string_view customCss) {
  const std::string_view css = customCss.empty() ? kDefaultDirectoryListingCss : customCss;
  const std::size_t escapedPathSize = HtmlEscapedSize(requestPath);
  const auto nDigitsEntries = ndigits(entries.size());

  // The body is allocated once, at its exact size.
  std::size_t bodySize = kListingHead.size() + escapedPathSize + kListingTitleEnd.size() + css.size() +
                         kListingStyleEnd.size() + escapedPathSize + kListingTableStart.size() +
                         kListingTableEnd.size() + kListingFooter.size();
  for (const auto& entry : entries) {
    bodySize += ListingRowSize(entry);
  }
  if (truncated) {
    bodySize += kListingTruncatedStart.size() + nDigitsEntries + kListingTruncatedEnd.size();
  }

  RawChars body(bodySize);
  char* pData = body.data();
  pData = Append(kListingHead, pData);
  pData = WriteHtmlEscaped(requestPath, pData);
  pData = Append(kListingTitleEnd, pData);
  pData = Append(css, pData);
  pData = Append(kListingStyleEnd, pData);
  pData = WriteHtmlEscaped(requestPath, pData);
  pData = Append(kListingTableStart, pData);
  for (const auto& entry : entries) {
    pData = WriteListingRow(entry, pData);
  }
  pData = Append(kListingTableEnd, pData);
  if (truncated) {
    pData = Append(kListingTruncatedStart, pData);
    pData = WriteUInt(pData, entries.size(), nDigitsEntries);
    pData = Append(kListingTruncatedEnd, pData);
  }
  pData = Append(kListingFooter, pData);

  assert(pData == body.data() + bodySize);
  body.setSize(bodySize);
  return body;
}

struct DirectoryListingResult {
  vector<DirectoryListingEntry> entries;
  bool truncated{false};
  bool isValid{false};
};

// Listing order: directories first, then by name.
bool ListedBefore(bool lhsIsDirectory, std::string_view lhsName, bool rhsIsDirectory, std::string_view rhsName) {
  if (lhsIsDirectory != rhsIsDirectory) {
    return lhsIsDirectory;
  }
  return lhsName < rhsName;
}

// Collect the entries of 'directory' to list, in the listing order: the first config.maxEntriesToList ones. A symbolic
// link is listed, and described by its target, only when 'isLinkServable' accepts it: a link answered 404 (leading
// outside of the root, or to a hidden name) is left out like a hidden file, its target not being described.
template <class IsLinkServable>
[[nodiscard]] DirectoryListingResult CollectDirectoryListing(const std::filesystem::path& directory,
                                                             const StaticFileConfig& config,
                                                             IsLinkServable isLinkServable) {
  const std::size_t limit = config.maxEntriesToList;

  DirectoryListingResult result;
  std::error_code ec;
  std::filesystem::directory_iterator it(directory, ec);
  if (ec) {
    log::error("Failed to open directory for listing '{}': {}", PathString(directory).c_str(), ec.message());
    return result;
  }

  const auto comp = [](const DirectoryListingEntry& lhs, const DirectoryListingEntry& rhs) {
    return ListedBefore(lhs.isDirectory, lhs.name, rhs.isDirectory, rhs.name);
  };

  // The order of exploration depends on the file system: the kept entries are the first ones in the listing order,
  // whatever it is. Once 'limit' entries are kept, they form a max-heap whose front is the last one: an entry listed
  // before it replaces it in O(log(limit)), any other one is dropped before its metadata is read.
  bool isHeap = false;
  for (const std::filesystem::directory_iterator end; it != end;) {
    std::filesystem::directory_entry current = *it;
    it.increment(ec);
    if (ec) {
      log::error("Failed to advance directory iterator for '{}': {}", PathString(directory).c_str(), ec.message());
      return result;
    }

    std::string name = current.path().filename().string();
    if (!config.showHiddenFiles && IsHiddenName(std::string_view(name))) {
      continue;
    }

    std::error_code stepEc;
    // The type of the entry itself, usually known from the directory read without system call.
    std::filesystem::file_status entryStatus = current.symlink_status(stepEc);
    if (!stepEc && std::filesystem::is_symlink(entryStatus)) {
      if (!isLinkServable(current.path())) {
        continue;
      }
      entryStatus = current.status(stepEc);  // of its target, inside of the root
    }
    if (stepEc) {
      log::error("Failed to get status for directory entry '{}': {}", PathString(current.path()).c_str(),
                 stepEc.message());
    }
    const bool isDirectory = !stepEc && std::filesystem::is_directory(entryStatus);

    if (result.entries.size() == limit) {
      result.truncated = true;
      if (limit == 0 ||
          !ListedBefore(isDirectory, name, result.entries.front().isDirectory, result.entries.front().name)) {
        continue;
      }
      std::ranges::pop_heap(result.entries, comp);
      result.entries.pop_back();
    }

    DirectoryListingEntry& info = result.entries.emplace_back(std::move(name));
    info.isDirectory = isDirectory;
    if (!stepEc && !isDirectory) {
      const auto fileSize = current.file_size(stepEc);
      if (stepEc) {
        log::error("Failed to get size for directory entry '{}': {}", PathString(current.path()).c_str(),
                   stepEc.message());
      } else {
        info.fileSizeKnown = true;
        info.sizeBytes = fileSize;
      }
    }

    const auto writeTime = current.last_write_time(stepEc);
    if (stepEc) {
      log::error("Failed to get last write time for directory entry '{}': {}", PathString(current.path()).c_str(),
                 stepEc.message());
    } else {
#if defined(__GLIBCXX__) || defined(_LIBCPP_VERSION)
      // file_clock::to_sys() of libstdc++ and libc++ is in their internal chrono headers, clock_cast() in <chrono>. The
      // file_clock of libc++ counts nanoseconds, its system_clock microseconds.
      info.lastModified =
          std::chrono::time_point_cast<SysClock::duration>(std::filesystem::file_time_type::clock::to_sys(writeTime));
#else
      // The file_clock of the MSVC STL converts through utc_clock only (aeronet/chrono-lite.hpp is <chrono> there).
      info.lastModified = std::chrono::clock_cast<SysClock>(writeTime);
#endif
    }

    info.entry = std::move(current);

    if (isHeap) {
      std::ranges::push_heap(result.entries, comp);
    } else if (result.entries.size() == limit) {
      std::ranges::make_heap(result.entries, comp);
      isHeap = true;
    }
  }

  result.isValid = true;
  if (isHeap) {
    std::ranges::sort_heap(result.entries, comp);
  } else {
    std::ranges::sort(result.entries, comp);
  }
  return result;
}

// Format a strong ETag ("<hex-size>-<hex-nanos>", quotes included) into 'buf', which must have room for at least
// StaticFileHandler::kMaxEtagSize bytes. Returns the number of bytes written.
std::uint8_t MakeStrongEtag(std::uint64_t fileSize, SysTimePoint lastModified, char* buf) {
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(lastModified.time_since_epoch()).count();

  static constexpr char kHexits[]{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

  std::uint8_t len = 0;

  // start with opening quote
  buf[len++] = '"';

  // helper to write minimal hex digits of 'value' directly into buf at current len
  auto write_hex_direct = [&](std::uint64_t value) {
    if (value == 0) {
      buf[len++] = '0';
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
      const uint8_t nibble = static_cast<uint8_t>((value >> shift) & 0xFU);
      assert(nibble < std::size(kHexits));
      buf[len++] = kHexits[nibble];
    }
  };

  // file size
  write_hex_direct(fileSize);
  // hyphen
  buf[len++] = '-';
  // nanoseconds timestamp
  write_hex_direct(static_cast<std::uint64_t>(nanos));
  // closing quote
  buf[len++] = '"';

  return len;
}

struct RangeSelection {
  // EmptyRepresentation: the only satisfiable range of an empty representation (a suffix range), which no
  // Content-Range can describe.
  enum class State : std::uint8_t { None, Valid, Invalid, Unsatisfiable, EmptyRepresentation };

  std::size_t offset{0};
  std::size_t length{0};
  std::uint32_t order{0};
  State state{State::None};
};

// Result of parsing a (possibly multi-range) Range header.
struct MultiRangeResult {
  enum class Kind : std::uint8_t { None, SingleValid, MultiValid, Invalid, Unsatisfiable, Rejected };

  Kind kind{Kind::None};
  vector<RangeSelection> ranges;
};

constexpr std::size_t kInvalidSize = std::numeric_limits<std::size_t>::max();
// Larger than any file: what a number beyond the range of std::size_t stands for.
constexpr std::size_t kSaturatedSize = kInvalidSize - 1U;

// The value of a 1*DIGIT position or length, kInvalidSize when 'token' is not one. RFC 9110 §14.1.1 sets no maximum:
// a value beyond the range of std::size_t saturates (to kSaturatedSize).
std::size_t ParseSize(std::string_view token) {
  assert(token == TrimOws(token));
  if (token.empty()) {
    return kInvalidSize;
  }
  std::size_t value;
  const auto first = token.data();
  const auto last = first + token.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ptr != last) {
    return kInvalidSize;  // not only digits (a non-digit first char leaves ptr at first)
  }
  // Only digits: an error can only be a value out of range.
  if (ec != std::errc{} || value == kInvalidSize) {
    return kSaturatedSize;
  }
  return value;
}

// Parse a single byte-range-spec (e.g. "0-99", "50-", "-100").
// Does NOT handle the "bytes=" prefix - caller must strip that.
// ETag comparisons in this file use strong validators. Weak ETags
// (prefixed with "W/") are treated as non-matching for strong comparisons. We
// derive ETags from file size and modification time and require exact (strong)
// matches for conditional semantics (304/412 decisions).
RangeSelection ParseSingleRangeSpec(std::string_view spec, std::size_t fileSize) {
  RangeSelection result;
  spec = TrimOws(spec);
  assert(!spec.empty());
  const auto dashPos = spec.find('-');
  if (dashPos == std::string_view::npos) {
    result.state = RangeSelection::State::Invalid;
    return result;
  }

  const std::string_view firstPart = spec.substr(0, dashPos);
  const std::string_view secondPart = spec.substr(dashPos + 1);
  if (!IsOwsTrimmed(firstPart) || !IsOwsTrimmed(secondPart)) {
    result.state = RangeSelection::State::Invalid;
    return result;
  }
  if (firstPart.empty()) {
    // suffix-byte-range-spec: bytes=-N (last N bytes)
    const std::size_t suffixLen = ParseSize(secondPart);
    if (suffixLen == kInvalidSize) {
      result.state = RangeSelection::State::Invalid;
      return result;
    }
    if (suffixLen == 0) {
      result.state = RangeSelection::State::Unsatisfiable;
      return result;
    }
    if (fileSize == 0) {
      // RFC 9110 §14.1.2: satisfiable, the whole (empty) representation.
      result.state = RangeSelection::State::EmptyRepresentation;
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
  if (secondPart.empty()) {
    if (firstValue >= fileSize) {
      result.state = RangeSelection::State::Unsatisfiable;
      return result;
    }
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
    result.state = RangeSelection::State::Invalid;
    return result;
  }
  if (firstValue >= fileSize) {
    result.state = RangeSelection::State::Unsatisfiable;
    return result;
  }
  const std::uint64_t endInclusive = std::min<std::uint64_t>(secondValue, fileSize - 1);
  result.offset = firstValue;
  result.length = endInclusive - firstValue + 1;
  result.state = RangeSelection::State::Valid;
  return result;
}

// Coalesce overlapping or adjacent ranges while preserving the order of the first matching range-spec.
void SortAndCoalesceRanges(vector<RangeSelection>& ranges) {
  if (ranges.size() <= 1) {
    return;
  }

  std::ranges::sort(ranges, {}, &RangeSelection::offset);

  uint32_t outIdx = 0;
  for (uint32_t idx = 1; idx < ranges.size(); ++idx) {
    const std::size_t curEnd = ranges[outIdx].offset + ranges[outIdx].length;
    if (ranges[idx].offset <= curEnd) {
      // Overlapping or adjacent - merge.
      const std::size_t newEnd = std::max(curEnd, ranges[idx].offset + ranges[idx].length);
      ranges[outIdx].length = newEnd - ranges[outIdx].offset;
      ranges[outIdx].order = std::min(ranges[outIdx].order, ranges[idx].order);
    } else {
      ++outIdx;
      ranges[outIdx] = ranges[idx];
    }
  }
  ranges.resize(outIdx + 1);
  std::ranges::sort(ranges, {}, &RangeSelection::order);
}

// Parse a (possibly multi-range) Range header value, returning a MultiRangeResult.
// maxRanges limits the number of non-empty range-specs to prevent abuse.
// Empty list elements are ignored as required by RFC 9110 §5.6.1.2, up to a bounded reasonable count.
// Unsatisfiable individual sub-ranges are silently dropped (RFC 9110 §14.1.1).
// If ALL sub-ranges are unsatisfiable, the result is Unsatisfiable.
// Any syntactically invalid sub-range makes the whole request Invalid.
// A satisfiable range of an empty representation cannot be described by a Content-Range: the Range is then ignored
// (None), as RFC 9110 §14.2 allows when the selected representation has no content.
MultiRangeResult ParseRanges(std::string_view raw, std::size_t fileSize, std::uint8_t maxRanges) {
  MultiRangeResult result;
  raw = TrimOws(raw);
  if (raw.empty()) {
    return result;
  }
  static constexpr std::string_view kBytesEqual = "bytes=";
  if (!CaseInsensitiveEqual(raw.substr(0, kBytesEqual.size()), kBytesEqual)) {
    // RFC 9110 §14.2 requires origin servers to ignore unsupported range units.
    return result;
  }
  raw.remove_prefix(kBytesEqual.size());
  raw = TrimOws(raw);
  if (raw.empty()) {
    result.kind = MultiRangeResult::Kind::Invalid;
    return result;
  }

  // Split on commas: each piece is one byte-range-spec
  vector<RangeSelection> satisfiable;
  std::size_t rangeCount = 0;
  std::uint8_t emptyRangeCount = 0;
  bool satisfiableByEmptyRepresentation = false;
  static constexpr std::uint8_t kMaxIgnoredEmptyRangeSpecs = 16;
  do {
    auto commaPos = raw.find(',');
    const auto spec = TrimOws((commaPos == std::string_view::npos) ? raw : raw.substr(0, commaPos));

    if (spec.empty()) {
      if (++emptyRangeCount > kMaxIgnoredEmptyRangeSpecs) {
        result.kind = MultiRangeResult::Kind::Rejected;
        return result;
      }
    } else {
      ++rangeCount;
      if (rangeCount > maxRanges) {
        result.kind = MultiRangeResult::Kind::Rejected;
        return result;
      }

      RangeSelection sel = ParseSingleRangeSpec(spec, fileSize);
      if (sel.state == RangeSelection::State::Invalid) {
        result.kind = MultiRangeResult::Kind::Invalid;
        return result;
      }
      if (sel.state == RangeSelection::State::Valid) {
        assert(rangeCount - 1 <= std::numeric_limits<std::uint32_t>::max());
        sel.order = static_cast<std::uint32_t>(rangeCount - 1);
        satisfiable.push_back(sel);
      } else if (sel.state == RangeSelection::State::EmptyRepresentation) {
        satisfiableByEmptyRepresentation = true;
      }
      // Unsatisfiable sub-ranges are silently dropped (RFC 9110 §14.1.1)
    }

    if (commaPos == std::string_view::npos) {
      break;
    }
    raw.remove_prefix(commaPos + 1);
  } while (!raw.empty());

  if (rangeCount == 0) {
    return result;
  }
  if (satisfiable.empty()) {
    if (!satisfiableByEmptyRepresentation) {
      result.kind = MultiRangeResult::Kind::Unsatisfiable;
    }
    return result;
  }

  SortAndCoalesceRanges(satisfiable);

  if (satisfiable.size() == 1) {
    result.kind = MultiRangeResult::Kind::SingleValid;
  } else {
    result.kind = MultiRangeResult::Kind::MultiValid;
  }
  result.ranges = std::move(satisfiable);
  return result;
}

enum class EtagComparison : std::uint8_t { Strong, Weak };

// Whether a member of an If-Match or If-None-Match list designates 'etag', the strong entity tag of the file (quotes
// included). The weak comparison (If-None-Match, RFC 9110 §13.1.2) ignores a W/ prefix. In the strong one (If-Match,
// §13.1.1), a weak tag never matches: it never equals 'etag', which starts with a quote.
bool EtagTokenMatches(std::string_view token, std::string_view etag, EtagComparison comparison) {
  token = TrimOws(token);
  // "*" matches any current entity tag (RFC 9110 §13.1.1).
  if (token == "*") {
    return true;
  }
  if (comparison == EtagComparison::Weak && token.starts_with(http::WeakETagPrefix)) {
    token.remove_prefix(http::WeakETagPrefix.size());
  }
  return token == etag;
}

bool EtagListMatches(std::string_view headerValue, std::string_view etag, EtagComparison comparison) {
  while (!headerValue.empty()) {
    const auto commaPos = headerValue.find(',');
    const auto token = commaPos == std::string_view::npos ? headerValue : headerValue.substr(0, commaPos);
    if (EtagTokenMatches(token, etag, comparison)) {
      return true;
    }
    if (commaPos == std::string_view::npos) {
      break;
    }
    headerValue.remove_prefix(commaPos + 1);
  }
  return false;
}

// Last-Modified has the one-second resolution of an HTTP-date, while 'lastModified' keeps the sub-second part of the
// file system: compared with a date, the validator is 'lastModified' truncated to the second (the value the client
// received). 'date' being a whole second, floor(lastModified) <= date is lastModified < date + 1 s, without division.
constexpr bool ModifiedNoLaterThan(SysTimePoint lastModified, SysTimePoint date) {
  return lastModified < date + std::chrono::seconds{1};
}

enum class ConditionalOutcome : std::uint8_t { None, NotModified, PreconditionFailed };

// Evaluate the preconditions in the order of RFC 9110 §13.2.2 (steps 1 to 4, If-Range being evaluated with the Range).
// 'etag' is the strong entity tag of the file, 'lastModified' its modification time.
ConditionalOutcome EvaluateConditionals(const HttpRequestView& request, std::string_view etag,
                                        SysTimePoint lastModified) {
  assert(!etag.empty() && lastModified != kInvalidTimePoint);

  if (auto ifMatch = request.headerValue(http::IfMatch); ifMatch.has_value()) {
    if (!EtagListMatches(*ifMatch, etag, EtagComparison::Strong)) {
      return ConditionalOutcome::PreconditionFailed;
    }
  } else if (auto ifUnmodified = request.headerValue(http::IfUnmodifiedSince); ifUnmodified.has_value()) {
    // Ignored with If-Match, or when not a valid HTTP-date (§13.1.4).
    const auto parsed = TryParseTimeRFC7231(*ifUnmodified);
    if (parsed != kInvalidTimePoint && !ModifiedNoLaterThan(lastModified, parsed)) {
      return ConditionalOutcome::PreconditionFailed;
    }
  }

  if (auto ifNoneMatch = request.headerValue(http::IfNoneMatch); ifNoneMatch.has_value()) {
    // If-Modified-Since is ignored with If-None-Match (§13.1.3).
    return EtagListMatches(*ifNoneMatch, etag, EtagComparison::Weak) ? ConditionalOutcome::NotModified
                                                                     : ConditionalOutcome::None;
  }

  if (auto ifModified = request.headerValue(http::IfModifiedSince); ifModified.has_value()) {
    const auto parsed = TryParseTimeRFC7231(*ifModified);
    if (parsed != kInvalidTimePoint && ModifiedNoLaterThan(lastModified, parsed)) {
      return ConditionalOutcome::NotModified;
    }
  }

  return ConditionalOutcome::None;
}

// RFC 9110 §13.1.5: the If-Range validator must exactly match the current one, entity tag (strong comparison: a weak
// tag never matches) or date (the Last-Modified value, at its one-second resolution).
bool IfRangeAllowsPartial(std::string_view value, std::string_view etag, SysTimePoint lastModified) {
  value = TrimOws(value);
  if (value.starts_with('"')) {
    return value == etag;
  }
  const auto parsed = TryParseTimeRFC7231(value);
  return parsed != kInvalidTimePoint && parsed <= lastModified && ModifiedNoLaterThan(lastModified, parsed);
}

constexpr std::string_view kBytesPrefixStr = "bytes ";
constexpr std::size_t kMaxRangeHeaderLen =
    kBytesPrefixStr.size() + (3UL * (std::numeric_limits<std::uint64_t>::digits10 + 1)) + 2UL;

char* BuildRangeHeader(std::uint64_t start, std::uint64_t length, std::uint64_t total, char* pData) {
  pData = AppendFixed<kBytesPrefixStr>(pData);

  pData = WriteUInt(pData, start, ndigits(start));
  *pData++ = '-';

  const auto middle = start + length - 1;
  pData = WriteUInt(pData, middle, ndigits(middle));
  *pData++ = '/';

  return WriteUInt(pData, total, ndigits(total));
}

constexpr std::string_view kUnsatisfiedRangePrefixStr = "bytes */";
constexpr std::size_t kMaxUnsatisfiedRangeHeaderLen =
    kUnsatisfiedRangePrefixStr.size() + std::numeric_limits<std::uint64_t>::digits10 + 1;

struct UnsatisfiedRangeHeaderBuf {
  static_assert(kMaxUnsatisfiedRangeHeaderLen <= std::numeric_limits<std::uint8_t>::max());

  char buf[kMaxUnsatisfiedRangeHeaderLen];
  std::uint8_t len;
};

UnsatisfiedRangeHeaderBuf BuildUnsatisfiedRangeHeader(std::uint64_t total) {
  UnsatisfiedRangeHeaderBuf result;
  char* buf = AppendFixed<kUnsatisfiedRangePrefixStr>(result.buf);

  buf = WriteUInt(buf, total, ndigits(total));
  assert(buf <= result.buf + kMaxUnsatisfiedRangeHeaderLen);
  result.len = static_cast<std::uint8_t>(buf - result.buf);

  return result;
}

constexpr uint32_t kBoundarySize = 24;
constexpr std::string_view kAeronetPrefix = "aeronet";

struct Boundary {
  char data[kBoundarySize];
};

// Generate a unique boundary string for multipart/byteranges responses.
// Format: "aeronet" + 16 hex chars (24 chars total).
Boundary GenerateBoundary() {
  // NOLINTNEXTLINE(cert-msc51-cpp) - thread_local PRNG seeded from random_device is intentional
  thread_local std::mt19937_64 rng(std::random_device{}());

  const std::uint64_t val = rng();
  static_assert(kBoundarySize >= kAeronetPrefix.size() + HexDigits(std::numeric_limits<decltype(val)>::max()),
                "Boundary size is too small");
  const auto nbZeros = kBoundarySize - kAeronetPrefix.size() - HexDigits(val);

  Boundary res;

  char* pData = AppendFixed<kAeronetPrefix>(res.data);
  std::memset(pData, '0', nbZeros);

  pData = ToLowerHex(val, pData + nbZeros);
  assert(pData == res.data + kBoundarySize);

  return res;
}

// Estimate the total size of a multipart/byteranges body (inclusive of all boundaries and per-part headers).
// Returns kInvalidSize if the estimate exceeds maxBodySize.
std::size_t EstimateMultipartBodySize(std::span<const RangeSelection> ranges, std::string_view contentType,
                                      std::size_t maxBodySize) {
  // Per part: "\r\n--" boundary "\r\nContent-Type: " contentType "\r\nContent-Range: bytes START-END/TOTAL\r\n\r\n"
  //           + <data bytes>
  // Closing:  "\r\n--" boundary "--\r\n"
  static constexpr std::size_t kMaxContentRangePerPart = 80;  // generous upper bound for "bytes START-END/TOTAL"
  const std::size_t perPartOverhead = 4 + kBoundarySize + 16 + contentType.size() + 17 + kMaxContentRangePerPart + 4;
  const std::size_t closingOverhead = 4 + kBoundarySize + 4;

  std::size_t total = closingOverhead;
  for (const auto& rng : ranges) {
    total += perPartOverhead + rng.length;
    if (total > maxBodySize) {
      return kInvalidSize;
    }
  }
  return total;
}

// Build the multipart/byteranges body by reading file segments and assembling the MIME structure.
// Returns an empty string on file read error or if the body would exceed maxBodySize.
bool BuildMultipartBody(const File& file, std::span<const RangeSelection> ranges, std::size_t fileSize,
                        std::string_view contentType, std::size_t maxBodySize, HttpResponse& resp) {
  const std::size_t estimate = EstimateMultipartBodySize(ranges, contentType, maxBodySize);
  if (estimate == kInvalidSize) {
    return false;
  }

  const Boundary boundary = GenerateBoundary();

  std::string_view boundarySv(boundary.data, kBoundarySize);

  resp.reserve(resp.sizeInlined() + estimate);

  static constexpr std::string_view kContentTypeHeaderPrefix = "multipart/byteranges; boundary=";

  char contentTypeHeader[kContentTypeHeaderPrefix.size() + kBoundarySize];
  char* endPtr = AppendFixed<kContentTypeHeaderPrefix>(contentTypeHeader);
  endPtr = Append(boundarySv, endPtr);

  assert(ranges.size() > 1U);

  static constexpr std::string_view kBoundaryPrefix = "\r\n--";
  static constexpr std::string_view kBoundarySuffix = "--\r\n";
  static constexpr std::string_view kContentTypeHeader = "\r\nContent-Type: ";
  static constexpr std::string_view kContentRangeHeader = "\r\nContent-Range: ";

  // Compute additional body size.
  std::size_t neededSize = kBoundaryPrefix.size();
  for (const auto& rng : ranges) {
    neededSize += boundarySv.size();
    neededSize += kContentTypeHeader.size() + contentType.size();
    neededSize += kContentRangeHeader.size() + kMaxRangeHeaderLen + http::DoubleCRLF.size();
    neededSize += rng.length;
    neededSize += kBoundaryPrefix.size();
  }

  neededSize += boundarySv.size() + kBoundarySuffix.size();

  bool error = false;
  resp.bodyInlineAppend(
      neededSize,
      [fileSize, contentType, boundarySv, &file, &ranges, &error](char* pInsert) {
        char* pData = AppendFixed<kBoundaryPrefix>(pInsert);

        for (const auto& rng : ranges) {
          pData = Append(boundarySv, pData);
          pData = AppendFixed<kContentTypeHeader>(pData);
          pData = Append(contentType, pData);
          pData = AppendFixed<kContentRangeHeader>(pData);

          pData = BuildRangeHeader(rng.offset, rng.length, fileSize, pData);
          pData = AppendFixed<http::DoubleCRLF>(pData);

          const std::size_t bytesRead =
              file.readAt(std::span<std::byte>(reinterpret_cast<std::byte*>(pData), rng.length), rng.offset);
          if (bytesRead != rng.length) {
            error = true;
            return static_cast<std::size_t>(0);
          }
          pData += bytesRead;

          pData = AppendFixed<kBoundaryPrefix>(pData);
        }

        pData = Append(boundarySv, pData);
        pData = AppendFixed<kBoundarySuffix>(pData);

        return static_cast<std::size_t>(pData - pInsert);
      },
      std::string_view(contentTypeHeader, endPtr));
  if (error) {
    resp.body(std::string_view{});
    return false;
  }

  resp.status(http::StatusCodePartialContent);

  return true;
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

StaticFileHandler::StaticFileHandler(const StaticFileHandler& rhs) : _root(rhs._root), _config(rhs._config) {
  // Do not copy the cache contents; the new instance starts with an empty cache.
}

StaticFileHandler& StaticFileHandler::operator=(const StaticFileHandler& rhs) {
  if (this != &rhs) [[likely]] {
    // Clear the current cache and reset LRU pointers.
    _headerCache.clear();
    _headerCachePool.clear();
    _lruHead = nullptr;
    _lruTail = nullptr;

    _root = rhs._root;
    _config = rhs._config;
  }
  return *this;
}

StaticFileHandler::ResolveResult StaticFileHandler::resolveTarget(const HttpRequestView& request,
                                                                  std::filesystem::path& resolvedPath) const {
  std::string_view rawPath = request.path();
  const bool requestedTrailingSlash = rawPath.back() == '/';

  rawPath.remove_prefix(1);

  std::filesystem::path relative;
  while (!rawPath.empty()) {
    const auto slashPos = rawPath.find('/');
    const auto segment = rawPath.substr(0, slashPos);
    if (!segment.empty() && segment != ".") {
      if (segment == ".." || (!_config.showHiddenFiles && IsHiddenName(segment))) {
        return ResolveResult::NotFound;
      }
#ifdef AERONET_WINDOWS
      // On Windows, a backslash separates path elements and a colon introduces a drive or an alternate data stream.
      if (segment.find_first_of("\\:") != std::string_view::npos) {
        return ResolveResult::NotFound;
      }
#endif
      relative /= segment;
    }
    if (slashPos == std::string_view::npos) {
      break;
    }
    rawPath.remove_prefix(slashPos + 1);
  }

  resolvedPath = _root / relative;
  // Symbolic links are followed: operator() then checks the real path of the target (isRealPathServable()).
  std::error_code ec;
  const auto status = std::filesystem::status(resolvedPath, ec);
  if (ec) {
    return ResolveResult::NotFound;
  }
  if (std::filesystem::is_directory(status)) {
    // A directory is served (its index file, or its listing) at the URL ending with a slash, against which the relative
    // references of the page resolve (RFC 3986 §5.2): '/dir' is redirected to '/dir/' first.
    if (!requestedTrailingSlash && _config.enableDirectoryIndex) {
      return ResolveResult::DirectoryRedirect;
    }
    if (!_config.defaultIndex().empty()) {
      std::filesystem::path indexPath = resolvedPath / _config.defaultIndex();
      std::error_code indexEc;
      const auto indexStatus = std::filesystem::status(indexPath, indexEc);
      if (!indexEc && std::filesystem::is_regular_file(indexStatus)) {
        if (!requestedTrailingSlash) {
          return ResolveResult::DirectoryRedirect;
        }
        resolvedPath = std::move(indexPath);
        return ResolveResult::RegularFile;
      }
    }
    return _config.enableDirectoryIndex ? ResolveResult::Directory : ResolveResult::NotFound;
  }

  // Only regular files are served: opening a FIFO or a device could block the event loop. The opened file is checked
  // again (File::isRegularFile()): the path may be replaced in between.
  return requestedTrailingSlash || !std::filesystem::is_regular_file(status) ? ResolveResult::NotFound
                                                                             : ResolveResult::RegularFile;
}

bool StaticFileHandler::isRealPathServable(const std::filesystem::path& path, const File* pFile,
                                           bool logRefusal) const {
  std::error_code ec;
  const auto canonicalPath = std::filesystem::canonical(path, ec);
  if (ec) {
    if (logRefusal) {
      log::error("Unable to resolve '{}': {}", PathString(path).c_str(), ec.message());
    }
    return false;
  }
  const auto& rootStr = _root.native();
  const auto& pathStr = canonicalPath.native();
  // '/srv/www' contains '/srv/www/a' but not '/srv/www2'.
  static constexpr auto kSep = std::filesystem::path::preferred_separator;
  if (!pathStr.starts_with(rootStr) ||
      (pathStr.size() != rootStr.size() && rootStr.back() != kSep && pathStr[rootStr.size()] != kSep)) {
    if (logRefusal) {
      log::warn("Refusing to serve '{}': it resolves outside of the root directory", PathString(path).c_str());
    }
    return false;
  }
  // The requested segments were checked, but the real path can reveal a hidden name that they did not show: a link to
  // a dotfile, or a Windows short name (GIT~1 for .git). The root directory itself may be hidden.
  if (!_config.showHiddenFiles && HasHiddenSegment(std::basic_string_view(pathStr).substr(rootStr.size()))) {
    if (logRefusal) {
      log::debug("Refusing to serve '{}': it resolves to a hidden file", PathString(path).c_str());
    }
    return false;
  }
  // The file was opened before its path was resolved here: a link retargeted meanwhile would make this check validate
  // another file than the one served. Listings check links without opening them.
  if (pFile != nullptr && !pFile->isAtPath(PathString(canonicalPath).c_str())) {
    log::warn("Refusing to serve '{}': it changed while being opened", PathString(path).c_str());
    return false;
  }
  return true;
}

void StaticFileHandler::buildHeaderMeta(std::string_view filePath, const File& file, CachedFileHeaders& out) const {
  out.fileSize = file.size();
  out.lastModified = file.lastModified();
  file.appendIdentityData(out.identity);
  // A successfully opened File always carries a valid mtime from its fstat(); operator() returns 503 before
  // reaching this point otherwise. So the modification time (and the derived ETag/date) are always well-formed.
  assert(out.lastModified != kInvalidTimePoint);

  out.etagLen = 0;
  if (_config.addEtag || _config.enableConditional) {
    out.etagLen = MakeStrongEtag(out.fileSize, out.lastModified, out.etag);
  }

  [[maybe_unused]] auto* end = TimeToStringRFC7231(out.lastModified, out.lastModifiedStr);
  assert(std::cmp_equal(end - out.lastModifiedStr, RFC7231DateStrLen));

  // Resolve content type:
  //  1) user-provided resolver (may return empty to defer),
  //  2) MIME type from the file extension,
  //  3) configured default content type.
  // Note: std::string_view is safe for use here, it will point to constant memory.
  std::string_view contentType;
  if (_config.contentTypeResolver) {
    contentType = _config.contentTypeResolver(filePath);
  }
  if (contentType.empty()) {
    contentType = DetermineMIMETypeStr(filePath);
    if (contentType.empty()) {
      contentType = _config.defaultContentType();
    }
  }

  out.pContentType = contentType.data();
  out.contentTypeLen = SafeCast<uint32_t>(contentType.size());
}

const StaticFileHandler::CachedFileHeaders* StaticFileHandler::resolveHeaderMeta(
    const std::filesystem::path& targetPath, std::string_view filePath, const File& file,
    CachedFileHeaders& scratch) const {
  if (_config.headerCacheCapacity == 0) {
    if (!isRealPathServable(targetPath, &file)) {
      return nullptr;
    }
    buildHeaderMeta(filePath, file, scratch);
    return &scratch;
  }

  auto it = _headerCache.find(filePath);
  if (it != _headerCache.end()) {
    CacheNode* pNode = it->second;
    CachedFileHeaders& headers = pNode->headers;

    // The root containment of this file was verified when it was cached. It only needs to be checked again if the
    // path now leads to another file (a symbolic link retargeted, a file replaced...).
    char identity[File::kIdentitySize];
    file.appendIdentityData(identity);
    const bool sameFile = std::memcmp(headers.identity, identity, sizeof(identity)) == 0;
    if (!sameFile && !isRealPathServable(targetPath, &file)) {
      return nullptr;
    }
    if (!sameFile || headers.fileSize != file.size() || headers.lastModified != file.lastModified()) {
      buildHeaderMeta(filePath, file, headers);
    }
    // Pool-owned node: address is stable, so this pointer surgery is safe regardless of what the map does
    // internally (rehash, backward-shift on other keys, etc).
    lruUnlink(pNode);
    lruPushFront(pNode);
    return &headers;
  }

  if (!isRealPathServable(targetPath, &file)) {
    return nullptr;
  }

  // Miss: evict first if we're already at the configured budget, then allocate. destroyAndRelease() returns the
  // freed slot to the pool's free-list, so steady-state churn (evict one, allocate one) does not grow the pool
  // further once it has reached 'headerCacheCapacity' live nodes - allocateAndConstruct() below just reuses it.
  if (_headerCachePool.size() >= _config.headerCacheCapacity) {
    CacheNode* pVictim = _lruTail;
    lruUnlink(pVictim);
    _headerCache.erase(pVictim->key);
    _headerCachePool.destroyAndRelease(pVictim);
  }

  CacheNode* pNode = _headerCachePool.allocateAndConstruct();
  pNode->key = RawChars32(filePath);
  buildHeaderMeta(filePath, file, pNode->headers);
  _headerCache.try_emplace(filePath, pNode);
  lruPushFront(pNode);
  return &pNode->headers;
}

HttpResponse StaticFileHandler::operator()(const HttpRequestView& request) const {
  HttpResponse resp = internal::HttpResponseAccess::MakeUnallocated();

  if (request.method() != http::Method::GET && request.method() != http::Method::HEAD) {
    static constexpr std::string_view kAllowedMethods = "GET, HEAD";
    resp = request.makeResponse(http::HeaderSize(http::Allow.size(), kAllowedMethods.size()),
                                http::StatusCodeMethodNotAllowed);
    internal::HttpResponseAccess::HeaderAddLineUnchecked(resp, http::Allow, kAllowedMethods);
    return resp;
  }

  const std::string_view requestPath = request.path();

  std::filesystem::path targetPath;
  const auto resolveResult = resolveTarget(request, targetPath);
  if (resolveResult == ResolveResult::NotFound) {
    resp = request.makeResponse(http::StatusCodeNotFound);
    return resp;
  }

  // resolveTarget already validated the target via status; no need to stat again.
  if (resolveResult == ResolveResult::Directory || resolveResult == ResolveResult::DirectoryRedirect) {
    // The redirect does not reveal a directory that would be answered 404 once redirected.
    if (!isRealPathServable(targetPath, nullptr)) {
      resp = request.makeResponse(http::StatusCodeNotFound);
      return resp;
    }

    static constexpr std::string_view kNoCache = "no-cache";
    if (resolveResult == ResolveResult::DirectoryRedirect) {
      static constexpr std::string_view kMovedPermanentlyBody = "Moved Permanently\n";
      static constexpr bool kAddSlash = true;

      // The path is percent-decoded: re-encoded so that the Location is a valid URI reference, the name of the
      // directory being able to hold any byte ('?', '%', CR, LF...), which could otherwise inject header lines.
      const std::size_t additionalSize =
          internal::HttpResponseAccess::LocationHeaderSize(requestPath, kAddSlash) +
          http::HeaderSize(http::CacheControl.size(), kNoCache.size()) +               // "no-cache"
          http::HeaderSize(http::ContentLength.size(), kMovedPermanentlyBody.size());  // "Moved Permanently\n"

      resp = request.makeResponse(additionalSize, http::StatusCodeMovedPermanently);
      internal::HttpResponseAccess::HeaderAddLocationUnchecked(resp, requestPath, kAddSlash);
      internal::HttpResponseAccess::HeaderAddLineUnchecked(resp, http::CacheControl, kNoCache);
      resp.body(kMovedPermanentlyBody);

      return resp;
    }

    assert(_config.enableDirectoryIndex);
    auto listing = CollectDirectoryListing(targetPath, _config, [this](const std::filesystem::path& linkPath) {
      return isRealPathServable(linkPath, nullptr, /*logRefusal=*/false);
    });
    if (!listing.isValid) {
      resp = request.makeResponse(http::StatusCodeInternalServerError);
      return resp;
    }

    static constexpr std::size_t kHeadersAdditionalSize = http::HeaderSize(http::CacheControl.size(), kNoCache.size()) +
                                                          http::HeaderSize(http::XDirectoryListingTruncated.size(), 1U);

    resp = request.makeResponse(kHeadersAdditionalSize + (128UL * listing.entries.size()), http::StatusCodeOK);
    resp.headerAddLine(http::CacheControl, kNoCache);
    resp.headerAddLine(http::XDirectoryListingTruncated, listing.truncated ? "1" : "0");

    static constexpr std::string_view kContentType = "text/html; charset=utf-8";
    if (_config.directoryIndexRenderer) {
      const auto rawEntries =
          std::make_unique_for_overwrite<std::filesystem::directory_entry[]>(listing.entries.size());
      std::transform(std::make_move_iterator(listing.entries.begin()), std::make_move_iterator(listing.entries.end()),
                     rawEntries.get(), [](DirectoryListingEntry&& entry) { return std::move(entry.entry); });

      resp.body(_config.directoryIndexRenderer(targetPath, std::span<const std::filesystem::directory_entry>(
                                                               rawEntries.get(), listing.entries.size())),
                kContentType);
    } else {
      resp.body(
          RenderDefaultDirectoryListing(requestPath, listing.entries, listing.truncated, _config.directoryListingCss()),
          kContentType);
    }
    return resp;
  }

  PathString pathString(targetPath);

  File file(pathString.c_str(), File::OpenMode::ReadOnly);
  if (!file) {
    // resolveTarget already confirmed the file exists, so failure here is a system error (e.g. EMFILE), not "not
    // found"
    resp = request.makeResponse(http::StatusCodeServiceUnavailable, "Unable to open file\n");
    return resp;
  }
  // Replaced since resolveTarget() checked its type (by a FIFO, opened without blocking, or a device).
  if (!file.isRegularFile()) {
    resp = request.makeResponse(http::StatusCodeNotFound);
    return resp;
  }

  const std::size_t fileSize = file.size();

  // Resolve the per-file formatted headers, reusing the cache when the file is unchanged. The file identity, size and
  // modification time captured by the File's fstat() double as the cache validation key, so the single stat()
  // performed at open time covers both serving and cache invalidation (no extra last_write_time() syscall).
  CachedFileHeaders scratchMeta;
  const CachedFileHeaders* pMeta = resolveHeaderMeta(targetPath, pathString.view, file, scratchMeta);
  if (pMeta == nullptr) {
    // Reached through a symbolic link leading outside of the root directory or to a hidden file, or replaced meanwhile.
    resp = request.makeResponse(http::StatusCodeNotFound);
    return resp;
  }
  const CachedFileHeaders& meta = *pMeta;

  const SysTimePoint lastModified = meta.lastModified;
  const std::string_view etagView{meta.etag, meta.etagLen};
  const std::string_view contentTypeForFile = meta.contentType();

  static constexpr std::string_view kBytes = "bytes";

  const bool useSmallFilesOptimization = fileSize <= _config.inlineFileThresholdBytes;

  const std::size_t additionalCapacity = 96UL + (useSmallFilesOptimization ? HttpResponse::BodySize(fileSize) : 0UL);

  const ConditionalOutcome conditionalOutcome =
      _config.enableConditional ? EvaluateConditionals(request, etagView, lastModified) : ConditionalOutcome::None;

  resp = request.makeResponse(additionalCapacity, http::StatusCodeNotFound);

  resp.headerAddLine(http::AcceptRanges, _config.enableRange ? kBytes : "none");

  // A 304 carries the ETag and the Vary of the 200 it stands for (RFC 9110 §15.4.5): the server compresses a file read
  // into the response like any body in memory, making its ETag weak.
  HttpRequestView::BodyCompressionOutlook notModifiedOutlook;
  if (conditionalOutcome == ConditionalOutcome::NotModified && useSmallFilesOptimization) {
    notModifiedOutlook = request.bodyCompressionOutlook(fileSize, contentTypeForFile);
  }
  if (_config.addEtag) {
    // ETag is always formatted when addEtag is set (a valid File guarantees a valid modification time).
    if (notModifiedOutlook.compressed) {
      char weakEtag[http::WeakETagPrefix.size() + kMaxEtagSize];
      char* pEnd = Append(etagView, Append(http::WeakETagPrefix, weakEtag));
      resp.headerAddLine(http::ETag, std::string_view(weakEtag, pEnd));
    } else {
      resp.headerAddLine(http::ETag, etagView);
    }
  }
  if (_config.addLastModified) {
    resp.headerAddLine(http::LastModified, std::string_view(meta.lastModifiedStr, RFC7231DateStrLen));
  }

  if (conditionalOutcome == ConditionalOutcome::NotModified) {
    if (notModifiedOutlook.varyAcceptEncoding) {
      resp.headerAddLine(http::Vary, http::AcceptEncoding);
    }
    resp.status(http::StatusCodeNotModified);
    return resp;
  }
  if (conditionalOutcome == ConditionalOutcome::PreconditionFailed) {
    resp.status(http::StatusCodePreconditionFailed);
    resp.body("Precondition Failed\n");
    return resp;
  }

  if (request.method() == http::Method::GET && _config.enableRange) {
    if (auto rangeHeaderVal = request.headerValue(http::Range); rangeHeaderVal.has_value()) {
      bool allowed = true;
      if (auto ifRange = request.headerValue(http::IfRange); ifRange.has_value()) {
        allowed = IfRangeAllowsPartial(*ifRange, etagView, lastModified);
      }
      if (allowed) {
        MultiRangeResult rangeResult = ParseRanges(*rangeHeaderVal, fileSize, _config.maxMultipartRanges);

        if (rangeResult.kind == MultiRangeResult::Kind::Invalid ||
            rangeResult.kind == MultiRangeResult::Kind::Unsatisfiable ||
            rangeResult.kind == MultiRangeResult::Kind::Rejected) {
          const auto unsatHeader = BuildUnsatisfiedRangeHeader(fileSize);
          resp.status(http::StatusCodeRangeNotSatisfiable);
          resp.headerAddLine(http::ContentRange, std::string_view(unsatHeader.buf, unsatHeader.len));
          resp.body(rangeResult.kind == MultiRangeResult::Kind::Invalid ? "Invalid Range\n"
                                                                        : "Range Not Satisfiable\n");
          return resp;
        }
        if (rangeResult.kind == MultiRangeResult::Kind::SingleValid) {
          const auto& sel = rangeResult.ranges[0];
          char buf[kMaxRangeHeaderLen];
          char* pEndRangeHeader = BuildRangeHeader(sel.offset, sel.length, fileSize, buf);

          resp.status(http::StatusCodePartialContent);
          resp.headerAddLine(http::ContentRange, std::string_view(buf, pEndRangeHeader));
          resp.file(std::move(file), sel.offset, sel.length, contentTypeForFile);
          return resp;
        }
        if (rangeResult.kind == MultiRangeResult::Kind::MultiValid &&
            BuildMultipartBody(file, rangeResult.ranges, fileSize, contentTypeForFile, _config.maxMultipartBodySize,
                               resp)) {
          return resp;
        }
        // Body exceeds maxMultipartBodySize or read error - fall through to full 200 response
      }
    }
  }

  resp.status(http::StatusCodeOK);

  // Small files: read into the response body (inline) so headers and body can be sent in a single write,
  // avoiding Nagle/delayed-ACK interaction and reducing syscall count. Large files use sendfile() for zero-copy.
  if (useSmallFilesOptimization) {
    std::size_t actualBytesRead = File::kError;
    resp.bodyInlineSet(
        fileSize,
        [&file, fileSize, &actualBytesRead](std::byte* buf) {
          actualBytesRead = file.readAt(std::span<std::byte>(buf, fileSize), 0);
          // Convert File::kError (SIZE_MAX) to 0 to signal bodyInlineSet that nothing was written
          return actualBytesRead == File::kError ? 0UL : actualBytesRead;
        },
        contentTypeForFile);

    if (actualBytesRead == File::kError || actualBytesRead != fileSize) {
      resp.status(http::StatusCodeInternalServerError);
      resp.body("File read error\n");
      return resp;
    }
  } else {
    resp.file(std::move(file), contentTypeForFile);
  }
  return resp;
}

void StaticFileHandler::lruUnlink(CacheNode* pNode) const {
  ((pNode->pPrev != nullptr) ? pNode->pPrev->pNext : _lruHead) = pNode->pNext;
  ((pNode->pNext != nullptr) ? pNode->pNext->pPrev : _lruTail) = pNode->pPrev;
  pNode->pPrev = pNode->pNext = nullptr;
}

void StaticFileHandler::lruPushFront(CacheNode* pNode) const {
  pNode->pPrev = nullptr;
  pNode->pNext = _lruHead;
  if (_lruHead != nullptr) {
    _lruHead->pPrev = pNode;
  }
  _lruHead = pNode;
  if (_lruTail == nullptr) {
    _lruTail = pNode;
  }
}

}  // namespace aeronet
