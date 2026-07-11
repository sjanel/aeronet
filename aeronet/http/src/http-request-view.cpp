#include "aeronet/http-request-view.hpp"

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

#include "aeronet/async-handler-state.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>
#include <functional>
#endif

#include "aeronet/connection-state.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/header-line-parse.hpp"
#include "aeronet/header-merge.hpp"
#include "aeronet/http-codec-result.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/major-minor-version.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/path-param-capture.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/template-constants.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/url-decode.hpp"
#include "http-method-parse.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/log-noexcept.hpp"
#endif

namespace aeronet {

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
void LogAsyncCallbackPostFailure(const char* what) noexcept {
  log_noexcept::error("Exception posting async callback: {}", what);
}
#endif

void HttpRequestView::QueryParamRange::iterator::advance() {
  _begKey = std::find(_begKey + 1, _endFullQuery, url::kNewPairSep);
  if (_begKey != _endFullQuery) {
    ++_begKey;
  }
}

HttpRequestView::QueryParam HttpRequestView::QueryParamRange::iterator::operator*() const {
  const char* commaPtr = std::find(_begKey + 1, _endFullQuery, url::kNewPairSep);
  const char* equalPtr = std::find(_begKey, commaPtr, url::kNewKeyValueSep);
  const char* keyEnd = (equalPtr == commaPtr) ? commaPtr : equalPtr;

  QueryParam ret(std::string_view{_begKey, keyEnd}, {});
  if (equalPtr != commaPtr) {
    ret.value = std::string_view(equalPtr + 1, commaPtr);
  }
  return ret;
}

std::string_view HttpRequestView::body() const {
  if (_bodyAccessMode == BodyAccessMode::Streaming) {
    throw std::logic_error("Cannot call body() after readBody() on the same request");
  }
  // Not the cleanest, but it should appear as const from the caller.
  auto& self = *const_cast<HttpRequestView*>(this);
  self._bodyAccessMode = BodyAccessMode::Aggregated;
  if (self._pBodyAccessBridge != nullptr && self._pBodyAccessBridge->aggregate != nullptr) {
    self._body = self._pBodyAccessBridge->aggregate(self, self._pBodyAccessContext);
  }
  return _body;
}

bool HttpRequestView::hasMoreBody() const {
  if (_bodyAccessMode == BodyAccessMode::Aggregated) {
    return false;
  }
  if (_pBodyAccessBridge == nullptr) {
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    // If an async handler started before the body was ready, the server will
    // set asyncState.needsBody and later install the aggregated bridge when
    // body bytes arrive. In that intermediate state, treat the request as
    // having more body so loops using hasMoreBody()+readBodyAsync() will
    // execute and suspend correctly.
    assert(_pOwnerState != nullptr);
    const auto* async = _pOwnerState->asyncState.get();
    return async != nullptr && async->active && async->needsBody;
#else
    return false;
#endif
  }
  return _pBodyAccessBridge->hasMore(*this, _pBodyAccessContext);
}

std::string_view HttpRequestView::readBody(std::size_t maxBytes) {
  if (_bodyAccessMode == BodyAccessMode::Aggregated) {
    throw std::logic_error("Cannot call readBody() after body() on the same request");
  }
  _bodyAccessMode = BodyAccessMode::Streaming;
  _activeStreamingChunk = _pBodyAccessBridge->readChunk(*this, _pBodyAccessContext, maxBytes);
  return _activeStreamingChunk;
}

[[nodiscard]] std::string_view HttpRequestView::alpnProtocol() const noexcept {
  assert(_pOwnerState != nullptr);
  return _pOwnerState->tlsInfo.selectedAlpn();
}

[[nodiscard]] std::string_view HttpRequestView::tlsCipher() const noexcept {
  assert(_pOwnerState != nullptr);
  return _pOwnerState->tlsInfo.negotiatedCipher();
}

[[nodiscard]] std::string_view HttpRequestView::tlsVersion() const noexcept {
  assert(_pOwnerState != nullptr);
  return _pOwnerState->tlsInfo.negotiatedVersion();
}

[[nodiscard]] std::string_view HttpRequestView::clientAddress() const noexcept {
  assert(_pOwnerState != nullptr);
  return _pOwnerState->clientAddress();
}

bool HttpRequestView::wantClose() const {
  return CaseInsensitiveEqual(headerValueOrEmpty(http::Connection), http::close);
}

HttpResponse HttpRequestView::makeResponse(std::size_t additionalCapacity, http::StatusCode statusCode) const {
  HttpResponse resp(additionalCapacity, statusCode, _pGlobalHeaders->fullStringWithLastSep(), {}, {},
                    HttpResponse::Check::No);
  resp._opts = makeResponseOptions();
  return resp;
}

HttpResponse HttpRequestView::makeResponse(std::string_view body, std::string_view contentType) const {
  HttpResponse resp(0UL, http::StatusCodeOK, _pGlobalHeaders->fullStringWithLastSep(), body, contentType,
                    HttpResponse::Check::No);
  resp._opts = makeResponseOptions();
  return resp;
}

HttpResponse HttpRequestView::makeResponse(http::StatusCode statusCode, std::string_view body,
                                           std::string_view contentType) const {
  HttpResponse resp(0UL, statusCode, _pGlobalHeaders->fullStringWithLastSep(), body, contentType,
                    HttpResponse::Check::No);
  resp._opts = makeResponseOptions();
  return resp;
}

HttpResponse HttpRequestView::makeResponse(std::span<const std::byte> body, std::string_view contentType) const {
  std::string_view asBody(reinterpret_cast<const char*>(body.data()), body.size());
  HttpResponse resp(0UL, http::StatusCodeOK, _pGlobalHeaders->fullStringWithLastSep(), asBody, contentType,
                    HttpResponse::Check::No);
  resp._opts = makeResponseOptions();
  return resp;
}

HttpResponse HttpRequestView::makeResponse(http::StatusCode statusCode, std::span<const std::byte> body,
                                           std::string_view contentType) const {
  std::string_view asBody(reinterpret_cast<const char*>(body.data()), body.size());
  HttpResponse resp(0UL, statusCode, _pGlobalHeaders->fullStringWithLastSep(), asBody, contentType,
                    HttpResponse::Check::No);
  resp._opts = makeResponseOptions();
  return resp;
}

bool HttpRequestView::hasExpectContinue() const noexcept {
  return version() == http::HTTP_1_1 && CaseInsensitiveEqual(headerValueOrEmpty(http::Expect), http::h100_continue);
}

bool HttpRequestView::isKeepAliveForHttp1(bool enableKeepAlive, uint32_t maxRequestsPerConnection,
                                          bool isServerRunning) const {
  if (!enableKeepAlive || _pOwnerState->requestsServed >= maxRequestsPerConnection || !isServerRunning) {
    return false;
  }
  const std::string_view connVal = headerValueOrEmpty(http::Connection);
  if (connVal.empty()) {
    // Default is keep-alive for HTTP/1.1, close for HTTP/1.0
    return version() == http::HTTP_1_1;
  }
  return !CaseInsensitiveEqual(connVal, http::close);
}

void HttpRequestView::init(const HttpServerConfig& config, internal::ResponseCompressionState& compressionState) {
  _pGlobalHeaders = &config.globalHeaders;
  _addTrailerHeader = config.addTrailerHeader;
  _addVaryAcceptEncoding = config.compression.addVaryAcceptEncodingHeader;
  _pCompressionState = &compressionState;
}

http::StatusCode HttpRequestView::initTrySetHead(std::span<char> inBuffer, RawChars& tmpBuffer,
                                                 uint32_t maxHeadersBytes, bool mergeAllowedForUnknownRequestHeaders,
                                                 tracing::SpanPtr traceSpan) {
  char* first = inBuffer.data();
  char* last = first + inBuffer.size();

  char* lineLast = SearchCRLF(first, last);
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

  if (!decodePath(first, nextSep)) {
    return http::StatusCodeBadRequest;
  }

  // Version
  first = nextSep + 1;
  _version = http::Version{first, static_cast<std::size_t>(lineLast - first)};
  if (!_version.isValid()) {
    // malformed version token
    return http::StatusCodeBadRequest;
  }
  if (_version.major() != 1 || _version.minor() > 1U) {
    return http::StatusCodeHTTPVersionNotSupported;
  }

  // Headers
  first = lineLast + http::CRLF.size();
  if (first == last) {
    // need more data - the headers are not complete yet
    return kStatusNeedMoreData;
  }

  _headers.clear();
  bool foundEmptyLine = false;
  for (; first < last; first = lineLast + http::CRLF.size()) {
    lineLast = SearchCRLF(first, last);
    if (lineLast == last) {
      // need more data - the headers are not complete yet
      return kStatusNeedMoreData;
    }
    if (lineLast == first) {
      // we are pointing to a CRLF (empty line) - end of headers
      foundEmptyLine = true;
      break;
    }
    if (std::cmp_less(maxHeadersBytes, lineLast + http::CRLF.size() - inBuffer.data())) {
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

  if (!foundEmptyLine) {
    // The for-loop exited because first >= last after advancing past the last header's CRLF,
    // without finding the empty line that terminates the header section.
    return kStatusNeedMoreData;
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
  _pBodyAccessBridge = nullptr;
  _pBodyAccessContext = nullptr;
  _trailers.clear();
  _pathParams.clear();
  _queryParams.clear();

  _headPinned = false;

  return http::StatusCodeOK;
}

void HttpRequestView::prefinalizeHttpResponse(HttpResponse& response, tracing::TelemetryContext& telemetryContext) {
  if (method() == http::Method::HEAD) {
    return;
  }
  if (response.status() == http::StatusCodeNotFound && !response.hasBody()) {
    response.bodyStatic(k404NotFoundTemplate2, http::ContentTypeTextHtml);
  }

  const Encoding encoding = responsePossibleEncoding();

  if (response.hasBodyInMemory() && encoding != Encoding::none) {
    const internal::CompressResponseResult result =
        internal::HttpCodec::TryCompressResponse(*_pCompressionState, encoding, response);

    switch (result) {
      case internal::CompressResponseResult::Uncompressed:
        break;
      case internal::CompressResponseResult::Compressed:
        telemetryContext.counterAdd("aeronet.http_responses.compression.total", 1);
        break;
      case internal::CompressResponseResult::ExceedsMaxRatio:
        telemetryContext.counterAdd("aeronet.http_responses.compression.exceeds_max_ratio_total", 1);
        break;
      default:
        assert(result == internal::CompressResponseResult::Error);
        telemetryContext.counterAdd("aeronet.http_responses.compression.errors_total", 1);
        break;
    }
  }
}

void HttpRequestView::finalizeBeforeHandlerCall(std::span<const PathParamCapture> pathParams) {
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
void HttpRequestView::pinHeadStorage(ConnectionState& state, AsyncHandlerStatePool& asyncStatePool) {
  if (_headPinned || _headSpanSize == 0) {
    return;
  }
  const char* oldBase = state.inBuffer.data();
  auto& asyncState = state.ensureAsyncState(asyncStatePool);
  asyncState.headBuffer.assign(oldBase, _headSpanSize);

  const auto remapPtr = [newBase = asyncState.headBuffer.data(), oldBase,
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

void HttpRequestView::shrinkAndMaybeClear() {
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

void HttpRequestView::end(http::StatusCode respStatusCode) {
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
void HttpRequestView::markAwaitingBody() const noexcept {
  auto* asyncState = _pOwnerState->asyncState.get();
  assert(asyncState != nullptr);
  assert(asyncState->active);
  asyncState->awaitReason = AsyncHandlerState::AwaitReason::WaitingForBody;
}

void HttpRequestView::markAwaitingCallback() const noexcept {
  if (_pH2SuspendedFlag != nullptr) {
    *_pH2SuspendedFlag = true;
    return;
  }
  auto* asyncState = _pOwnerState->asyncState.get();
  assert(asyncState != nullptr);
  assert(asyncState->active);
  asyncState->awaitReason = AsyncHandlerState::AwaitReason::WaitingForCallback;
}

void HttpRequestView::postCallback(std::coroutine_handle<> handle, std::function<void()> work) const {
  if (_h2PostCallback) {
    _h2PostCallback(handle, std::move(work));
    return;
  }
  auto* asyncState = _pOwnerState->asyncState.get();
  assert(asyncState != nullptr);
  assert(asyncState->active);
  assert(asyncState->postCallback);
  asyncState->postCallback(handle, std::move(work));
}
#endif

HttpResponse::Options HttpRequestView::makeResponseOptions() const noexcept {
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

bool HttpRequestView::decodePath(char* pathStart, char* pathEnd) {
  char* questionMark = std::find(pathStart, pathEnd, '?');
  if (questionMark != pathEnd) {
    const char* paramsEnd = url::DecodeQueryParamsInPlace(questionMark + 1, pathEnd);
    _pDecodedQueryParams = questionMark + 1;
    _decodedQueryParamsLength = SafeCast<uint32_t>(paramsEnd - _pDecodedQueryParams);
  } else {
    _decodedQueryParamsLength = 0;
  }
  const char* pathLast = url::DecodeInPlace(pathStart, questionMark);
  if (pathLast == nullptr || pathStart == pathLast) {
    return false;
  }
  _pPath = pathStart;
  _pathLength = SafeCast<uint32_t>(pathLast - pathStart);
  return true;
}

}  // namespace aeronet