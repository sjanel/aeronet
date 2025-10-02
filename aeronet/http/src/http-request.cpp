#include "aeronet/http-request.hpp"

#include <algorithm>    // std::find
#include <cctype>       // std::tolower
#include <cstddef>      // std::size_t
#include <iterator>     // std::distance
#include <optional>     // std::optional
#include <string_view>  // std::string_view
#include <utility>      // std::make_pair

#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-status-code.hpp"  // http::StatusCode values
#include "aeronet/http-version.hpp"      // http::parseHttpVersion
#include "connection-state.hpp"          // ConnectionState parameter
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

[[nodiscard]] bool HttpRequest::wantClose() const {
  std::string_view connVal = headerValueOrEmpty(http::Connection);
  if (connVal.size() == http::close.size()) {
    for (std::size_t iChar = 0; iChar < http::close.size(); ++iChar) {
      const char lhs = tolower(connVal[iChar]);
      const char rhs = static_cast<char>(http::close[iChar]);
      if (lhs != rhs) {
        return false;
      }
    }
    return true;
  }
  return false;
}

http::StatusCode HttpRequest::setHead(ConnectionState& state, std::size_t maxHeadersBytes) {
  auto* first = state.buffer.data();
  auto* last = first + state.buffer.size();

  /* Example HTTP request:

    GET /path HTTP/1.1\r\n
    Host: example.com\r\n
    User-Agent: FooBar\r\n
    \r\n
  */

  // Although the HTTP standard specifies that requests should have CRLF '\r\n' as line separators,
  // some clients may send requests only with '\n'. We tolerate lone LF in parsing.
  auto* lineLast = std::find(first, last, '\n');
  if (lineLast == last || std::distance(first, lineLast) < 2) {
    return http::StatusCodeBadRequest;
  }
  auto* nextSep = std::find(first, lineLast, ' ');
  if (nextSep == lineLast) {
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

  // Version (allow trailing CR; parseHttpVersion tolerates it via from_chars behavior)
  first = nextSep + 1;
  if (!http::parseHttpVersion(first, lineLast, _version)) {
    return http::StatusCodeBadRequest;  // malformed version token
  }
  if (_version.major != 1 || _version.minor > 1U) {
    return http::StatusCodeHTTPVersionNotSupported;
  }

  // Headers
  first = lineLast + 1;
  auto* headersFirst = first;
  while (first < last) {
    lineLast = std::find(first, last, '\n');
    if (lineLast == last) {  // need more data for complete header line
      return http::StatusCodeBadRequest;
    }
    // Detect blank line (CRLF or LF terminator) signaling end of headers.
    if (first == lineLast || (std::distance(first, lineLast) == 1 && *first == '\r')) {
      break;  // end of headers
    }
    nextSep = std::find(first, lineLast, ':');
    if (nextSep == lineLast) {
      return http::StatusCodeBadRequest;
    }

    const auto isSpace = [](char ch) { return ch == ' ' || ch == '\t'; };

    // Spaces are possible (as of ' ' or '\t') as trailing and leading around the value, but not within the key.
    auto* valueFirst = nextSep + 1;
    while (valueFirst < lineLast && isSpace(*valueFirst)) {
      ++valueFirst;
    }
    auto* valueLast = lineLast;
    if (*(valueLast - 1) == '\r') {
      --valueLast;
    }
    while (valueLast > valueFirst && isSpace(*(valueLast - 1))) {
      --valueLast;
    }

    // Store header as "Key\037Value\0" in _headers string for later parsing.
    // TODO: support duplicate header keys
    _headers.emplace(std::make_pair(std::string_view(first, nextSep), std::string_view(valueFirst, valueLast)));

    first = lineLast + 1;
  }

  // Parsed double CRLF
  lineLast = std::find(first, last, '\n');
  if (lineLast == last) {
    return http::StatusCodeBadRequest;
  }
  _flatHeaders = std::string_view(headersFirst, lineLast + 1);

  if (_flatHeaders.size() > maxHeadersBytes + 2UL) {
    return http::StatusCodeRequestHeaderFieldsTooLarge;
  }

  // Propagate negotiated ALPN (if any) from connection state into per-request object.
  _alpnProtocol = state.selectedAlpn;
  _tlsCipher = state.negotiatedCipher;
  _tlsVersion = state.negotiatedVersion;

  return http::StatusCodeOK;
}

}  // namespace aeronet