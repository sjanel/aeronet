#include "aeronet/http-request.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>
#include <functional>
#endif

#include "aeronet/connection-state.hpp"
#include "aeronet/header-line-parse.hpp"
#include "aeronet/header-merge.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/major-minor-version.hpp"
#include "aeronet/path-param-capture.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/safe-cast.hpp"
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
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    // If an async handler started before the body was ready, the server will
    // set asyncState.needsBody and later install the aggregated bridge when
    // body bytes arrive. In that intermediate state, treat the request as
    // having more body so loops using hasMoreBody()+readBodyAsync() will
    // execute and suspend correctly.
    assert(_ownerState != nullptr);
    const auto& async = _ownerState->asyncState;
    return async.active && async.needsBody;
#else
    return false;
#endif
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

[[nodiscard]] std::string_view HttpRequest::alpnProtocol() const noexcept {
  assert(_ownerState != nullptr);
  return _ownerState->tlsInfo.selectedAlpn();
}

[[nodiscard]] std::string_view HttpRequest::tlsCipher() const noexcept {
  assert(_ownerState != nullptr);
  return _ownerState->tlsInfo.negotiatedCipher();
}

[[nodiscard]] std::string_view HttpRequest::tlsVersion() const noexcept {
  assert(_ownerState != nullptr);
  return _ownerState->tlsInfo.negotiatedVersion();
}

bool HttpRequest::wantClose() const { return CaseInsensitiveEqual(headerValueOrEmpty(http::Connection), http::close); }

HttpResponse HttpRequest::makeResponse(std::size_t additionalCapacity, http::StatusCode statusCode) const {
  HttpResponse resp(additionalCapacity, statusCode, _pGlobalHeaders->fullStringWithLastSep(), {}, {},
                    HttpResponse::Check::No);
  resp._opts = makeResponseOptions();
  return resp;
}

HttpResponse HttpRequest::makeResponse(std::string_view body, std::string_view contentType) const {
  HttpResponse resp(0UL, http::StatusCodeOK, _pGlobalHeaders->fullStringWithLastSep(), body, contentType,
                    HttpResponse::Check::No);
  resp._opts = makeResponseOptions();
  return resp;
}

HttpResponse HttpRequest::makeResponse(http::StatusCode statusCode, std::string_view body,
                                       std::string_view contentType) const {
  HttpResponse resp(0UL, statusCode, _pGlobalHeaders->fullStringWithLastSep(), body, contentType,
                    HttpResponse::Check::No);
  resp._opts = makeResponseOptions();
  return resp;
}

HttpResponse HttpRequest::makeResponse(std::span<const std::byte> body, std::string_view contentType) const {
  std::string_view asBody(reinterpret_cast<const char*>(body.data()), body.size());
  HttpResponse resp(0UL, http::StatusCodeOK, _pGlobalHeaders->fullStringWithLastSep(), asBody, contentType,
                    HttpResponse::Check::No);
  resp._opts = makeResponseOptions();
  return resp;
}

HttpResponse HttpRequest::makeResponse(http::StatusCode statusCode, std::span<const std::byte> body,
                                       std::string_view contentType) const {
  std::string_view asBody(reinterpret_cast<const char*>(body.data()), body.size());
  HttpResponse resp(0UL, statusCode, _pGlobalHeaders->fullStringWithLastSep(), asBody, contentType,
                    HttpResponse::Check::No);
  resp._opts = makeResponseOptions();
  return resp;
}

bool HttpRequest::hasExpectContinue() const noexcept {
  return version() == http::HTTP_1_1 && CaseInsensitiveEqual(headerValueOrEmpty(http::Expect), http::h100_continue);
}

bool HttpRequest::isKeepAliveForHttp1(bool enableKeepAlive, uint32_t maxRequestsPerConnection,
                                      bool isServerRunning) const {
  if (!enableKeepAlive || _ownerState->requestsServed >= maxRequestsPerConnection || !isServerRunning) {
    return false;
  }
  const std::string_view connVal = headerValueOrEmpty(http::Connection);
  if (connVal.empty()) {
    // Default is keep-alive for HTTP/1.1, close for HTTP/1.0
    return version() == http::HTTP_1_1;
  }
  return !CaseInsensitiveEqual(connVal, http::close);
}

void HttpRequest::init(const HttpServerConfig& config, internal::ResponseCompressionState& compressionState) {
  _pGlobalHeaders = &config.globalHeaders;
  _addTrailerHeader = config.addTrailerHeader;
  _addVaryAcceptEncoding = config.compression.addVaryAcceptEncodingHeader;
  _pCompressionState = &compressionState;
}

http::StatusCode HttpRequest::initTrySetHead(std::span<char> inBuffer, RawChars& tmpBuffer, std::size_t maxHeadersBytes,
                                             bool mergeAllowedForUnknownRequestHeaders, tracing::SpanPtr traceSpan) {
  char* first = inBuffer.data();
  char* last = first + inBuffer.size();

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
    _pDecodedQueryParams = questionMark + 1;
    _decodedQueryParamsLength = SafeCast<uint32_t>(paramsEnd - _pDecodedQueryParams);
  } else {
    _decodedQueryParamsLength = 0;
  }
  const char* pathLast = url::DecodeInPlace(first, questionMark);
  if (pathLast == nullptr || first == pathLast) {
    return http::StatusCodeBadRequest;
  }
  _pPath = first;
  _pathLength = SafeCast<uint32_t>(pathLast - first);

  // Version
  first = nextSep + 1;
  _version = http::Version{std::string_view{first, lineLast}};
  if (!_version.isValid()) {
    // malformed version token
    return http::StatusCodeBadRequest;
  }
  if (_version.major() != 1 || _version.minor() > 1U) {
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
    if (maxHeadersBytes < static_cast<std::size_t>(lineLast - headersFirst) + http::CRLF.size()) {
      return http::StatusCodeRequestHeaderFieldsTooLarge;
    }
    const auto [nameView, valueView] = http::ParseHeaderLine(first, lineLast);
    if (nameView.empty() || std::ranges::any_of(nameView, [](char ch) { return http::IsHeaderWhitespace(ch); })) {
      return http::StatusCodeBadRequest;
    }

    // Store header using in-place merge helper (headers live inside connection buffer).
    if (!http::AddOrMergeHeaderInPlace(_headers, nameView, valueView, tmpBuffer, inBuffer.data(), first,
                                       mergeAllowedForUnknownRequestHeaders)) {
      return http::StatusCodeBadRequest;
    }
  }

  // At this point, we have a complete request head, and we point to a CRLF.

  if (traceSpan) {
    traceSpan->setAttribute("http.method", http::MethodToStr(_method));
    traceSpan->setAttribute("http.target", path());
    traceSpan->setAttribute("http.scheme", "http");

    const auto hostIt = _headers.find("Host");
    if (hostIt != _headers.end()) {
      traceSpan->setAttribute("http.host", hostIt->second);
    }
  }

  _traceSpan = std::move(traceSpan);
  _headSpanSize = static_cast<std::size_t>(first + http::CRLF.size() - inBuffer.data());

  _body = {};
  _activeStreamingChunk = {};
  _bodyAccessMode = BodyAccessMode::Undecided;
  _bodyAccessBridge = nullptr;
  _bodyAccessContext = nullptr;
  _trailers.clear();
  _pathParams.clear();
  _queryParams.clear();

  _headPinned = false;

  return http::StatusCodeOK;
}

void HttpRequest::finalizeBeforeHandlerCall(std::span<const PathParamCapture> pathParams) {
  // Populate path params map view from router captures
  _pathParams.clear();
  for (const auto& capture : pathParams) {
    _pathParams.emplace(capture.key, capture.value);
  }
  _queryParams.clear();
  for (const auto& [key, value] : queryParamsRange()) {
    _queryParams.insert_or_assign(key, value);
  }
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
void HttpRequest::pinHeadStorage(ConnectionState& state) {
  if (_headPinned || _headSpanSize == 0) {
    return;
  }
  const char* oldBase = state.inBuffer.data();
  state.asyncState.headBuffer.assign(oldBase, _headSpanSize);

  const auto remapPtr = [newBase = state.asyncState.headBuffer.data(), oldBase,
                         oldLimit = oldBase + _headSpanSize](const char* ptr) -> const char* {
    if (ptr < oldBase || ptr >= oldLimit) {
      return ptr;
    }
    return newBase + static_cast<std::size_t>(ptr - oldBase);
  };

  _pPath = remapPtr(_pPath);
  _pDecodedQueryParams = remapPtr(_pDecodedQueryParams);

  auto remapMap = [&remapPtr](auto& map) {
    for (auto& [key, val] : map) {
      key = {remapPtr(key.data()), key.size()};
      val = {remapPtr(val.data()), val.size()};
    }
  };

  remapMap(_headers);
  remapMap(_trailers);
  remapMap(_pathParams);

  _headPinned = true;
}
#endif

void HttpRequest::shrinkAndMaybeClear() {
  // we cannot simply rehash(0) for std::string_view maps because if the maps are not empty,
  // rehashing would call the hash function on the string_views, which may point to
  // deallocated memory if the connection buffer was shrunk. So we check the load factor and if it's low,
  // we clear the map to free memory.
  auto shrinkMap = [](auto& map) {
    if (!map.empty()) {
      if (map.load_factor() < 0.25F) {
        map = {};  // clear and free memory
      }
    }
  };

  shrinkMap(_headers);
  shrinkMap(_trailers);
  shrinkMap(_pathParams);
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

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
void HttpRequest::markAwaitingBody() const noexcept {
  assert(_ownerState->asyncState.active);
  _ownerState->asyncState.awaitReason = ConnectionState::AsyncHandlerState::AwaitReason::WaitingForBody;
}

void HttpRequest::markAwaitingCallback() const noexcept {
  assert(_ownerState->asyncState.active);
  _ownerState->asyncState.awaitReason = ConnectionState::AsyncHandlerState::AwaitReason::WaitingForCallback;
}

void HttpRequest::postCallback(std::coroutine_handle<> handle, std::function<void()> work) const {
  assert(_ownerState->asyncState.active);
  assert(_ownerState->asyncState.postCallback);
  _ownerState->asyncState.postCallback(handle, std::move(work));
}
#endif

HttpResponse::Options HttpRequest::makeResponseOptions() const noexcept {
  assert(_pCompressionState != nullptr);
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  HttpResponse::Options opts(*_pCompressionState, _responsePossibleEncoding);
  opts.addVaryAcceptEncoding(_addVaryAcceptEncoding);
#else
  HttpResponse::Options opts;
#endif
  opts.close(wantClose());
  opts.addTrailerHeader(_addTrailerHeader);
  opts.headMethod(method() == http::Method::HEAD);
  opts.setPrepared();
  return opts;
}

}  // namespace aeronet