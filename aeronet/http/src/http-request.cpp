#include "aeronet/http-request.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "connection-state.hpp"
#include "header-line-parse.hpp"
#include "header-merge.hpp"
#include "major-minor-version.hpp"
#include "raw-chars.hpp"
#include "string-equal-ignore-case.hpp"
#include "toupperlower.hpp"
#include "url-decode.hpp"

namespace aeronet {

void HttpRequest::QueryParamRange::iterator::advance() {
  _begKey = std::find(_begKey + 1, _endFullQuery, url::kNewPairSep);
  if (_begKey != _endFullQuery) {
    ++_begKey;
  }
}

HttpRequest::QueryParam HttpRequest::QueryParamRange::iterator::operator*() const {
  const char* commaPtr = std::find(_begKey + 1, _endFullQuery, url::kNewPairSep);
  const char* equalPtr = std::find(_begKey, commaPtr, url::kNewKeyValueSep);
  const char* keyEnd = (equalPtr == commaPtr) ? commaPtr : equalPtr;

  QueryParam ret(std::string_view{_begKey, keyEnd}, {});
  if (equalPtr != commaPtr) {
    ret.value = std::string_view(equalPtr + 1, commaPtr);
  }
  return ret;
}

std::string_view HttpRequest::headerValueOrEmpty(std::string_view headerKey) const noexcept {
  const auto it = headers().find(headerKey);
  if (it != headers().end()) {
    return it->second;
  }
  return {};
}

std::optional<std::string_view> HttpRequest::headerValue(std::string_view headerKey) const noexcept {
  const auto it = headers().find(headerKey);
  if (it != headers().end()) {
    return it->second;
  }
  return {};
}

bool HttpRequest::wantClose() const {
  std::string_view connectionValue = headerValueOrEmpty(http::Connection);
  if (connectionValue.size() == http::close.size()) {
    for (std::size_t iChar = 0; iChar < http::close.size(); ++iChar) {
      const char lhs = tolower(connectionValue[iChar]);
      const char rhs = static_cast<char>(http::close[iChar]);
      if (lhs != rhs) {
        return false;
      }
    }
    return true;
  }
  return false;
}

bool HttpRequest::hasExpectContinue() const noexcept {
  return version() == http::HTTP_1_1 && CaseInsensitiveEqual(headerValueOrEmpty(http::Expect), http::h100_continue);
}

http::StatusCode HttpRequest::initTrySetHead(ConnectionState& state, RawChars& tmpBuffer, std::size_t maxHeadersBytes,
                                             bool mergeAllowedForUnknownRequestHeaders, tracing::SpanPtr traceSpan) {
  auto* first = state.inBuffer.data();
  auto* last = first + state.inBuffer.size();

  _reqStart = std::chrono::steady_clock::now();

  // Example : GET /path HTTP/1.1\r\nHost: example.com\r\nUser-Agent: FooBar\r\n\r\n

  // Although the HTTP standard specifies that requests should have CRLF '\r\n' as line separators,
  // some clients may send requests only with '\n'. We tolerate lone LF in parsing.
  auto* lineLast = std::find(first, last, '\n');
  if (lineLast == last) {
    // not enough data
    return 0;
  }
  if (std::cmp_less(std::distance(first, lineLast), http::kHttpReqLineMinLen - 1UL)) {
    return http::StatusCodeBadRequest;
  }
  auto* nextSep = std::find(first, lineLast, ' ');
  if (nextSep == lineLast) {
    // we have a new line, but no spaces in the first line. This is definitely a bad request.
    return http::StatusCodeBadRequest;
  }

  // Method
  auto optMethod = http::toMethodEnum(std::string_view(first, nextSep));
  if (!optMethod) {
    return http::StatusCodeNotImplemented;
  }
  _method = *optMethod;

  // Path
  first = nextSep + 1;
  nextSep = std::find(first, lineLast, ' ');
  auto* questionMark = std::find(first, nextSep, '?');
  char* pathLast;
  if (questionMark != nextSep) {
    auto paramsEnd = url::DecodeQueryParamsInPlace(questionMark + 1, nextSep);
    _decodedQueryParams = std::string_view(questionMark + 1, paramsEnd);
    pathLast = url::DecodeInPlace(first, questionMark);
  } else {
    _decodedQueryParams = {};
    pathLast = url::DecodeInPlace(first, nextSep);
  }
  if (pathLast == nullptr) {
    return http::StatusCodeBadRequest;
  }
  _path = std::string_view(first, pathLast);

  // Version (allow trailing CR; parseVersion tolerates it via from_chars behavior)
  first = nextSep + 1;
  if (!parseVersion(first, lineLast, _version)) {
    // malformed version token
    return http::StatusCodeBadRequest;
  }
  if (_version.major != 1 || _version.minor > 1U) {
    return http::StatusCodeHTTPVersionNotSupported;
  }

  // Headers
  first = lineLast + 1;
  auto* headersFirst = first;

  _headers.clear();
  while (first < last) {
    lineLast = std::find(first, last, '\n');
    if (lineLast == last) {  // need more data for complete header line
      return 0;
    }
    if (std::cmp_less(maxHeadersBytes, std::distance(headersFirst, lineLast))) {
      return http::StatusCodeRequestHeaderFieldsTooLarge;
    }
    // Detect blank line (CRLF or LF terminator) signaling end of headers.
    if (first == lineLast || (std::distance(first, lineLast) == 1 && *first == '\r')) {
      break;  // end of headers
    }
    auto [nameView, valueView] = http::parseHeaderLine(first, lineLast);
    if (nameView.empty()) {
      return http::StatusCodeBadRequest;
    }

    // Store header using in-place merge helper (headers live inside connection buffer).
    if (!http::AddOrMergeHeaderInPlace(_headers, nameView, valueView, tmpBuffer, state.inBuffer.data(), first,
                                       mergeAllowedForUnknownRequestHeaders)) {
      return http::StatusCodeBadRequest;
    }
    first = lineLast + 1;
  }

  if (traceSpan) {
    traceSpan->setAttribute("http.method", http::toMethodStr(_method));
    traceSpan->setAttribute("http.target", _path);
    traceSpan->setAttribute("http.scheme", "http");

    const auto hostIt = _headers.find("Host");
    if (hostIt != _headers.end()) {
      traceSpan->setAttribute("http.host", hostIt->second);
    }
  }

  _traceSpan = std::move(traceSpan);

  // Parsed double CRLF
  lineLast = std::find(first, last, '\n');
  if (lineLast == last) {
    return http::StatusCodeBadRequest;
  }
  _flatHeaders = std::string_view(headersFirst, lineLast + 1);

  // Propagate negotiated ALPN (if any) from connection state into per-request object.
  _alpnProtocol = state.tlsInfo.selectedAlpn();
  _tlsCipher = state.tlsInfo.negotiatedCipher();
  _tlsVersion = state.tlsInfo.negotiatedVersion();

  _body = {};
  _trailers.clear();
  _pathParams.clear();

  return http::StatusCodeOK;
}

void HttpRequest::shrink_to_fit() {
  _headers.rehash(0);
  _trailers.rehash(0);
  _pathParams.rehash(0);
}

void HttpRequest::end(http::StatusCode respStatusCode) {
  // End the span after response is finalized
  if (_traceSpan) {
    const auto reqEnd = std::chrono::steady_clock::now();
    const auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(reqEnd - reqStart());

    _traceSpan->setAttribute("http.status_code", respStatusCode);
    _traceSpan->setAttribute("http.duration_us", durationUs.count());

    _traceSpan->end();
    _traceSpan.reset();
  }
}

}  // namespace aeronet