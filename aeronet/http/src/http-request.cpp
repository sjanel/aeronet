#include "aeronet/http-request.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/header-line-parse.hpp"
#include "aeronet/header-merge.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/major-minor-version.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/url-decode.hpp"
#include "http-method-parse.hpp"

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

std::string_view HttpRequest::body() const {
  if (_bodyAccessMode == BodyAccessMode::Streaming) {
    throw std::logic_error("Cannot call body() after readBody() on the same request");
  }
  // Not the cleanest, but it should appear as const from the caller.
  auto& self = *const_cast<HttpRequest*>(this);
  self._bodyAccessMode = BodyAccessMode::Aggregated;
  if (self._bodyAccessBridge != nullptr && self._bodyAccessBridge->aggregate != nullptr) {
    self._body = self._bodyAccessBridge->aggregate(self, self._bodyAccessContext);
  }
  return _body;
}

bool HttpRequest::hasMoreBody() const {
  if (_bodyAccessMode == BodyAccessMode::Aggregated) {
    return false;
  }
  if (_bodyAccessBridge == nullptr) {
    // If an async handler started before the body was ready, the server will
    // set asyncState.needsBody and later install the aggregated bridge when
    // body bytes arrive. In that intermediate state, treat the request as
    // having more body so loops using hasMoreBody()+readBodyAsync() will
    // execute and suspend correctly.
    if (_ownerState != nullptr) {
      const auto& async = _ownerState->asyncState;
      if (async.active && async.needsBody) {
        return true;
      }
    }
    return false;
  }
  return _bodyAccessBridge->hasMore(*this, _bodyAccessContext);
}

std::string_view HttpRequest::readBody(std::size_t maxBytes) {
  if (_bodyAccessMode == BodyAccessMode::Aggregated) {
    throw std::logic_error("Cannot call readBody() after body() on the same request");
  }
  _bodyAccessMode = BodyAccessMode::Streaming;
  _activeStreamingChunk = _bodyAccessBridge->readChunk(*this, _bodyAccessContext, maxBytes);
  return _activeStreamingChunk;
}

bool HttpRequest::wantClose() const { return CaseInsensitiveEqual(headerValueOrEmpty(http::Connection), http::close); }

bool HttpRequest::hasExpectContinue() const noexcept {
  return version() == http::HTTP_1_1 && CaseInsensitiveEqual(headerValueOrEmpty(http::Expect), http::h100_continue);
}

http::StatusCode HttpRequest::initTrySetHead(ConnectionState& state, RawChars& tmpBuffer, std::size_t maxHeadersBytes,
                                             bool mergeAllowedForUnknownRequestHeaders, tracing::SpanPtr traceSpan) {
  char* first = state.inBuffer.data();
  char* last = first + state.inBuffer.size();
  _headPinned = false;

  _reqStart = std::chrono::steady_clock::now();

  char* lineLast = std::search(first, last, http::CRLF.begin(), http::CRLF.end());
  if (lineLast == last) {
    return kStatusNeedMoreData;
  }
  if (std::cmp_less(lineLast - first, http::kHttpReqLineMinLen - http::CRLF.size())) {
    return http::StatusCodeBadRequest;
  }
  char* nextSep = std::find(first, lineLast, ' ');
  if (nextSep == lineLast) {
    // we have a new line, but no spaces in the first line. This is definitely a bad request.
    return http::StatusCodeBadRequest;
  }

  // Method
  const auto optMethod = http::MethodStrToOptEnum(std::string_view(first, nextSep));
  if (!optMethod) {
    return http::StatusCodeNotImplemented;
  }
  _method = *optMethod;

  // Path
  first = nextSep + 1;
  nextSep = std::find(first, lineLast, ' ');
  char* questionMark = std::find(first, nextSep, '?');
  if (questionMark != nextSep) {
    const char* paramsEnd = url::DecodeQueryParamsInPlace(questionMark + 1, nextSep);
    _decodedQueryParams = std::string_view(questionMark + 1, paramsEnd);
  } else {
    _decodedQueryParams = {};
  }
  const char* pathLast = url::DecodeInPlace(first, questionMark);
  if (pathLast == nullptr || first == pathLast) {
    return http::StatusCodeBadRequest;
  }
  _path = std::string_view(first, pathLast);

  // Version (allow trailing CR; parseVersion tolerates it via from_chars behavior)
  first = nextSep + 1;
  if (!ParseVersion(first, lineLast, _version)) {
    // malformed version token
    return http::StatusCodeBadRequest;
  }
  if (_version.major != 1 || _version.minor > 1U) {
    return http::StatusCodeHTTPVersionNotSupported;
  }

  // Headers
  auto* headersFirst = lineLast + http::CRLF.size();
  if (headersFirst == last) {
    // need more data - the headers are not complete yet
    return kStatusNeedMoreData;
  }

  _headers.clear();
  for (first = headersFirst; first < last; first = lineLast + http::CRLF.size()) {
    lineLast = std::search(first, last, http::CRLF.begin(), http::CRLF.end());
    if (lineLast == last) {
      // need more data - the headers are not complete yet
      return kStatusNeedMoreData;
    }
    if (lineLast == first) {
      // we are pointing to a CRLF (empty line) - end of headers
      break;
    }
    if (std::cmp_less(maxHeadersBytes, static_cast<std::size_t>(lineLast - headersFirst) + http::CRLF.size())) {
      return http::StatusCodeRequestHeaderFieldsTooLarge;
    }
    const auto [nameView, valueView] = http::ParseHeaderLine(first, lineLast);
    if (nameView.empty() || std::ranges::any_of(nameView, [](char ch) { return http::IsHeaderWhitespace(ch); })) {
      return http::StatusCodeBadRequest;
    }

    // Store header using in-place merge helper (headers live inside connection buffer).
    if (!http::AddOrMergeHeaderInPlace(_headers, nameView, valueView, tmpBuffer, state.inBuffer.data(), first,
                                       mergeAllowedForUnknownRequestHeaders)) {
      return http::StatusCodeBadRequest;
    }
  }

  // At this point, we have a complete request head, and we point to a CRLF.

  if (traceSpan) {
    traceSpan->setAttribute("http.method", http::MethodToStr(_method));
    traceSpan->setAttribute("http.target", _path);
    traceSpan->setAttribute("http.scheme", "http");

    const auto hostIt = _headers.find("Host");
    if (hostIt != _headers.end()) {
      traceSpan->setAttribute("http.host", hostIt->second);
    }
  }

  _traceSpan = std::move(traceSpan);
  _headSpanSize = static_cast<std::size_t>(first + http::CRLF.size() - state.inBuffer.data());

  // Propagate negotiated ALPN (if any) from connection state into per-request object.
  _alpnProtocol = state.tlsInfo.selectedAlpn();
  _tlsCipher = state.tlsInfo.negotiatedCipher();
  _tlsVersion = state.tlsInfo.negotiatedVersion();

  _body = {};
  _activeStreamingChunk = {};
  _bodyAccessMode = BodyAccessMode::Undecided;
  _bodyAccessBridge = nullptr;
  _bodyAccessContext = nullptr;
  _trailers.clear();
  _pathParams.clear();

  return http::StatusCodeOK;
}

void HttpRequest::pinHeadStorage(ConnectionState& state) {
  if (_headPinned || _headSpanSize == 0) {
    return;
  }
  const char* oldBase = state.inBuffer.data();
  RawChars& storage = state.headBuffer;
  storage.clear();
  storage.append(oldBase, _headSpanSize);
  const char* newBase = storage.data();
  const char* oldLimit = oldBase + _headSpanSize;

  auto remap = [&](std::string_view view) -> std::string_view {
    if (view.empty()) {
      return view;
    }
    const char* begin = view.data();
    if (begin < oldBase || begin >= oldLimit) {
      return view;
    }
    const auto offset = static_cast<std::size_t>(begin - oldBase);
    return {newBase + offset, view.size()};
  };

  _path = remap(_path);
  _decodedQueryParams = remap(_decodedQueryParams);

  auto remapMap = [&](auto& map) {
    for (auto& entry : map) {
      entry.first = remap(entry.first);
      entry.second = remap(entry.second);
    }
  };

  remapMap(_headers);
  remapMap(_trailers);
  remapMap(_pathParams);

  _headPinned = true;
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

void HttpRequest::markAwaitingBody() const noexcept {
  assert(_ownerState->asyncState.active);
  _ownerState->asyncState.awaitReason = ConnectionState::AsyncHandlerState::AwaitReason::WaitingForBody;
}

}  // namespace aeronet