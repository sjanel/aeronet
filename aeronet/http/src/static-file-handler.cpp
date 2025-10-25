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
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
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
#include "invalid_argument_exception.hpp"
#include "ndigits.hpp"
#include "raw-chars.hpp"
#include "string-equal-ignore-case.hpp"
#include "stringconv.hpp"
#include "timedef.hpp"
#include "timestring.hpp"

namespace aeronet {
namespace {

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
  resp.addCustomHeader(http::LastModified, std::string_view(buf.data(), len));
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
  if (raw.find(',') != std::string_view::npos) {
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
  http::StatusCode status{http::StatusCodeOK};
  bool rangeAllowed{true};
};

[[nodiscard]] ConditionalOutcome evaluateConditionals(const HttpRequest& request, bool isGetOrHead,
                                                      std::string_view etag, SysTimePoint lastModified) {
  if (etag.empty() && lastModified == kInvalidTimePoint) {
    return {};
  }

  ConditionalOutcome outcome;

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
      return outcome;
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
    throw invalid_argument("StaticFileHandler root must be an existing directory");
  }
}

bool StaticFileHandler::resolveTarget(const HttpRequest& request, std::filesystem::path& resolvedPath) const {
  std::string_view rawPath = request.path();
  if (rawPath.empty()) {
    rawPath = "/";
  }
  bool requestedTrailingSlash = rawPath.ends_with('/');
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
  const bool isDirectory = std::filesystem::is_directory(status);
  if (isDirectory || requestedTrailingSlash) {
    if (_config.defaultIndex.empty()) {
      return false;
    }
    resolvedPath /= _config.defaultIndex;
    return true;
  }
  return true;
}

HttpResponse StaticFileHandler::makeError(http::StatusCode code, std::string_view reason) {
  HttpResponse resp(code);
  if (!reason.empty()) {
    resp.reason(reason);
  }
  resp.contentType(http::ContentTypeTextPlain);

  RawChars body(reason.size() + 1UL);
  body.unchecked_append(reason);
  body.unchecked_push_back('\n');

  resp.body(std::move(body));
  return resp;
}

HttpResponse StaticFileHandler::operator()(const HttpRequest& request) const {
  const bool isGet = request.method() == http::Method::GET;
  const bool isHead = request.method() == http::Method::HEAD;
  if (!isGet && !isHead) {
    HttpResponse resp(http::StatusCodeMethodNotAllowed, http::ReasonMethodNotAllowed);
    resp.addCustomHeader(http::Allow, "GET, HEAD");
    resp.contentType(http::ContentTypeTextPlain);
    resp.body("Method Not Allowed\n");
    return resp;
  }

  std::filesystem::path targetPath;
  if (!resolveTarget(request, targetPath)) {
    return makeError(http::StatusCodeNotFound, http::NotFound);
  }

  std::error_code ec;
  const auto status = std::filesystem::symlink_status(targetPath, ec);
  if (ec || !std::filesystem::exists(status) || !std::filesystem::is_regular_file(status)) {
    return makeError(http::StatusCodeNotFound, http::NotFound);
  }

  const auto fileSize = std::filesystem::file_size(targetPath, ec);
  if (ec) {
    return makeError(http::StatusCodeNotFound, http::NotFound);
  }

  SysTimePoint lastModified = kInvalidTimePoint;
  if ((_config.addLastModified || _config.enableConditional) && !ec) {
    const auto writeTime = std::filesystem::last_write_time(targetPath, ec);
    if (!ec) {
      lastModified = std::chrono::clock_cast<SysClock>(writeTime);
    }
  }

  File file;
  try {
    file = File(targetPath.string(), File::OpenMode::ReadOnly);
  } catch (const std::exception&) {
    return makeError(http::StatusCodeNotFound, http::NotFound);
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
      resp.contentType(http::ContentTypeTextPlain);
      if (!etagView.empty()) {
        resp.addCustomHeader(http::ETag, etagView);
      }
      if (_config.addLastModified && lastModified != kInvalidTimePoint) {
        addLastModifiedHeader(resp, lastModified);
      }
      resp.addCustomHeader(http::AcceptRanges, "bytes");
      resp.body("Precondition Failed\n");
      return resp;
    }
    if (conditionalOutcome.kind == ConditionalOutcome::Kind::NotModified) {
      HttpResponse resp(http::StatusCodeNotModified, "Not Modified");
      if (!etagView.empty()) {
        resp.addCustomHeader(http::ETag, etagView);
      }
      if (_config.addLastModified && lastModified != kInvalidTimePoint) {
        addLastModifiedHeader(resp, lastModified);
      }
      resp.addCustomHeader(http::AcceptRanges, "bytes");
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
    resp.contentType(http::ContentTypeTextPlain);
    const auto rangeHeader = buildUnsatisfiedRangeHeader(fileSize);
    resp.addCustomHeader(http::ContentRange, std::string_view(rangeHeader.buf.data(), rangeHeader.len));
    resp.addCustomHeader(http::AcceptRanges, "bytes");
    resp.body("Invalid Range\n");
    return resp;
  }

  if (rangeSelection.state == RangeSelection::State::Unsatisfiable) {
    HttpResponse resp(http::StatusCodeRangeNotSatisfiable, "Range Not Satisfiable");
    resp.contentType(http::ContentTypeTextPlain);
    const auto rangeHeader = buildUnsatisfiedRangeHeader(fileSize);
    resp.addCustomHeader(http::ContentRange, std::string_view(rangeHeader.buf.data(), rangeHeader.len));
    resp.addCustomHeader(http::AcceptRanges, "bytes");
    resp.body("Range Not Satisfiable\n");
    return resp;
  }

  HttpResponse resp(http::StatusCodeOK, http::ReasonOK);
  resp.addCustomHeader(http::AcceptRanges, "bytes");
  if (!etagView.empty()) {
    resp.addCustomHeader(http::ETag, etagView);
  }
  if (_config.addLastModified && lastModified != kInvalidTimePoint) {
    addLastModifiedHeader(resp, lastModified);
  }

  if (_config.contentTypeResolver) {
    if (auto resolved = _config.contentTypeResolver(targetPath.generic_string()); !resolved.empty()) {
      resp.contentType(std::move(resolved));
    } else {
      resp.contentType(_config.defaultContentType);
    }
  } else {
    resp.contentType(_config.defaultContentType);
  }

  if (rangeSelection.state == RangeSelection::State::Valid) {
    resp.statusCode(http::StatusCodePartialContent).reason("Partial Content");
    const auto rangeHeader = buildRangeHeader(rangeSelection.offset, rangeSelection.length, fileSize);
    resp.addCustomHeader(http::ContentRange, std::string_view(rangeHeader.buf.data(), rangeHeader.len));
    resp.file(std::move(file), static_cast<std::size_t>(rangeSelection.offset),
              static_cast<std::size_t>(rangeSelection.length));
    return resp;
  }

  resp.file(std::move(file));
  return resp;
}

}  // namespace aeronet
