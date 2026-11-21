// The requests of the scripts (fetch(), XMLHttpRequest, navigator.sendBeacon()) over the HTTP client of the page, with
// the rules of the Fetch Standard (forbidden headers, CORS, credentials, Referer), and the text and URL helpers of the
// prelude.

#include "js-network.hpp"

#include <quickjs.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-message-common.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/internal/web-session.hpp"
#include "aeronet/log.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/web-client-config.hpp"
#include "cors.hpp"
#include "html-node-ops.hpp"
#include "js-engine.hpp"
#include "js-prelude.hpp"
#include "page.hpp"
#include "request-queue.hpp"
#include "web-url.hpp"

namespace aeronet::internal {

namespace {

// The arguments of request() and requestSync(), in this order (see the prelude): the method (normalized), the URL (to
// resolve), the header lines ("name:value\n", lower-case names sorted, values combined, Content-Type included), the
// body (null, a string or a Uint8Array), the mode and the credentials (RequestMode, RequestCredentials), and for
// request(), the callback of the response.
enum RequestArgument : uint8_t { kMethod, kUrl, kHeaders, kBody, kMode, kCredentials, kCallback };

constexpr int kRequestArgCount = kCallback;

// The native memory of a queued request, besides its URL, headers and body.
constexpr std::size_t kRequestMemory = sizeof(RequestQueue::Request) + 256;

// The size of the headers that aeronet adds to a request of a script, besides their values: Accept, Origin, Referer,
// Content-Type and Content-Length, or those of a preflight.
constexpr std::size_t kExtraHeadersSize = 160;

// An ArrayBuffer of a copy of 'bytes', JS_EXCEPTION on memory exhaustion.
JSValue NewArrayBuffer(JSContext* pCtx, std::string_view bytes) noexcept {
  static constexpr uint8_t kNoByte = 0;
  return JS_NewArrayBufferCopy(pCtx, bytes.empty() ? &kNoByte : reinterpret_cast<const uint8_t*>(bytes.data()),
                               bytes.size());
}

// The header lines of a request of a script, parsed from the text of request() ("name:value\n" each).
class HeaderLines {
 public:
  explicit HeaderLines(std::string_view lines) noexcept : _lines(lines) {}

  // Reads the next header into 'name' and 'value'. Returns false when none is left.
  bool next(std::string_view& name, std::string_view& value) noexcept {
    if (_lines.empty()) {
      return false;
    }
    const std::size_t end = _lines.find('\n');
    const std::string_view line = _lines.substr(0, end);
    _lines = end == std::string_view::npos ? std::string_view{} : _lines.substr(end + 1);
    const std::size_t colon = line.find(':');
    name = line.substr(0, colon);
    value = colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
    return true;
  }

 private:
  std::string_view _lines;
};

// The elements of 'array', a non-empty Uint8Array given by the prelude.
std::string_view Uint8ArrayView(JSContext* pCtx, JSValueConst array) noexcept {
  std::size_t size = 0;
  const uint8_t* pData = JS_GetUint8Array(pCtx, &size, array);
  assert(pData != nullptr);
  return {reinterpret_cast<const char*>(pData), size};
}

// An int32 argument given by the prelude (a mode, an id).
int32_t IntArgument(JSValueConst value) noexcept {
  assert(JS_VALUE_GET_TAG(value) == JS_TAG_INT);
  return JS_VALUE_GET_INT(value);
}

// The bytes of the body argument of request(): none for null, the UTF-8 of a string, the elements of a non-empty
// Uint8Array (the prelude converts the other bodies). A local object, like JsString.
class BodyBytes {
 public:
  BodyBytes(JSContext* pCtx, JSValueConst body) noexcept : _pCtx(pCtx) {
    if (JS_IsString(body)) {
      std::size_t size = 0;
      _pStr = JS_ToCStringLen(pCtx, &size, body);
      _failed = _pStr == nullptr;
      _bytes = _failed ? std::string_view{} : std::string_view(_pStr, size);
    } else if (!JS_IsNull(body)) {
      _bytes = Uint8ArrayView(pCtx, body);
    }
  }

  BodyBytes(const BodyBytes&) = delete;
  BodyBytes& operator=(const BodyBytes&) = delete;
  BodyBytes(BodyBytes&&) = delete;
  BodyBytes& operator=(BodyBytes&&) = delete;

  ~BodyBytes() { JS_FreeCString(_pCtx, _pStr); }  // nothing for a null pointer

  // Whether the conversion of a string body failed (memory exhaustion), its exception pending.
  [[nodiscard]] bool failed() const noexcept { return _failed; }

  [[nodiscard]] std::string_view view() const noexcept { return _bytes; }

 private:
  JSContext* _pCtx;
  const char* _pStr{};  // the C string of a string body
  std::string_view _bytes;
  bool _failed{false};
};

// What request() and requestSync() do with the request of their arguments: queue it with its callback, or send it.
using RequestUse = JSValue (*)(Page& page, RequestQueue::Request&& request, JSValueConst callback);

// The request of the arguments of request() or requestSync(), given to use(page, request, callback) whose result is
// returned. JS_EXCEPTION, with a TypeError thrown, for a request that fetch() refuses: an unsupported method or URL, a
// request to another origin in the same-origin mode, an unsafe one in the no-cors mode, a body with GET or HEAD, an
// invalid header.
JSValue MakeRequest(JSContext* pCtx, JSValueConst* argv, RequestUse use, JSValueConst callback) {
  Page& page = Page::Of(pCtx);
  const JsString methodArg(pCtx, argv[kMethod]);
  if (methodArg.failed()) {
    return JS_EXCEPTION;
  }
  const http::Method method = http::ParseMethodStr(methodArg.view());
  if (method == http::kMethodInvalid || method == http::Method::CONNECT || method == http::Method::TRACE) {
    return JS_ThrowTypeError(pCtx, "aeronet: the HTTP method '%s' is not supported", methodArg.view().data());
  }
  const JsString urlArg(pCtx, argv[kUrl]);
  if (urlArg.failed()) {
    return JS_EXCEPTION;
  }
  // In the buffer of the URL resolver, until the next resolution: none before the request copies it. Absolute, as
  // resolved by the prelude (Request, XMLHttpRequest.open()).
  const std::string_view url = WithoutFragment(page.resolveUrl(urlArg.view()));
  assert(!url.empty());
  if (!IsHttpUrl(url) || !HttpRequest::IsValidUrl(url)) {
    return JS_ThrowTypeError(pCtx, "aeronet: only http and https URLs can be fetched, not '%.*s'",
                             static_cast<int>(url.size()), url.data());
  }
  const std::string_view origin = page.origin();
  const bool crossOrigin = !IsOfOrigin(url, origin);
  const auto requestMode = static_cast<RequestMode>(IntArgument(argv[kMode]));
  if (requestMode == RequestMode::NoCors && !IsCorsSafelistedMethod(method)) {
    return JS_ThrowTypeError(pCtx, "aeronet: the method '%s' is not allowed in the no-cors mode",
                             methodArg.view().data());
  }
  if (requestMode == RequestMode::SameOrigin && crossOrigin) {
    return JS_ThrowTypeError(pCtx,
                             "Failed to fetch '%.*s': its origin is not that of the document in the "
                             "same-origin mode",
                             static_cast<int>(url.size()), url.data());
  }
  const JsString lines(pCtx, argv[kHeaders]);
  if (lines.failed()) {
    return JS_EXCEPTION;
  }
  const BodyBytes bodyBytes(pCtx, argv[kBody]);
  if (bodyBytes.failed()) {
    return JS_EXCEPTION;
  }
  const std::string_view body = bodyBytes.view();
  // Refused by the Request constructor, dropped by XMLHttpRequest.send().
  assert(body.empty() || (method != http::Method::GET && method != http::Method::HEAD));
  const std::string_view referrer = ReferrerOf(page.url(), origin, url);
  const bool sendsOrigin = (crossOrigin && requestMode == RequestMode::Cors) ||
                           (method != http::Method::GET && method != http::Method::HEAD);
  // The lines become headers of about their size, with room for Accept, Origin, Referer and the body headers: no
  // reallocation of the request.
  const std::size_t capacity = lines.view().size() + referrer.size() + origin.size() + kExtraHeadersSize + body.size();

  RequestQueue::Request request{
      page.session().http.makeRequest(capacity, method, url),
      {},
      JS_UNDEFINED,
      kRequestMemory + url.size() + capacity,
      requestMode,
      static_cast<RequestCredentials>(IntArgument(argv[kCredentials])),
      crossOrigin,
      crossOrigin && requestMode == RequestMode::Cors && !IsCorsSafelistedMethod(method),
  };
  std::string_view contentType;
  bool accept = false;
  std::string_view name;
  std::string_view value;
  HeaderLines headers(lines.view());
  while (headers.next(name, value)) {
    if (!http::IsValidLowerCaseHeaderName(name) || !http::IsValidHeaderValue(value)) {
      return JS_ThrowTypeError(pCtx, "aeronet: invalid header '%.*s: %.*s'", static_cast<int>(name.size()), name.data(),
                               static_cast<int>(value.size()), value.data());
    }
    if (IsForbiddenRequestHeader(name, value)) {
      continue;  // set by the browser: ignored, as by the Headers of a request
    }
    // The values of a name are combined, each within 128 bytes when safelisted: the total of the safelisted values
    // stays below 1024 bytes (Fetch "CORS-unsafe request-header names").
    if (!IsCorsSafelistedRequestHeader(name, value)) {
      request.corsHeaders.append(request.corsHeaders.empty() ? std::string_view{} : std::string_view(","));
      request.corsHeaders.append(name);
    }
    accept = accept || name == "accept";
    if (name == "content-type") {
      if (!body.empty() && !IsValidContentType(value)) {
        return JS_ThrowTypeError(pCtx, "aeronet: unsupported Content-Type '%.*s'", static_cast<int>(value.size()),
                                 value.data());
      }
      contentType = value;
    } else {
      request.http.header(LowerAsciiKey(name), value);
    }
  }
  if (requestMode == RequestMode::NoCors && !request.corsHeaders.empty()) {
    return JS_ThrowTypeError(pCtx, "aeronet: a no-cors request only takes CORS-safelisted headers, not '%.*s'",
                             static_cast<int>(request.corsHeaders.size()), request.corsHeaders.data());
  }
  request.preflight =
      request.preflight || (crossOrigin && requestMode == RequestMode::Cors && !request.corsHeaders.empty());
  if (!accept) {
    request.http.header(http::Accept, "*/*");
  }
  if (sendsOrigin) {
    request.http.headerAddLine("origin", origin);
  }
  if (!referrer.empty()) {
    request.http.headerAddLine("referer", referrer);
  }
  if (!body.empty()) {
    request.http.body(body, contentType.empty() ? http::ContentTypeApplicationOctetStream : contentType);
  }
  return use(page, std::move(request), callback);
}

// The response of the scripts for 'result', the response of 'request' sent by 'page': [status, statusText, url,
// redirected, type, headers (names and values in turn), body (ArrayBuffer)], or JS_NULL for a network error with its
// reason in 'error'. JS_EXCEPTION on memory exhaustion.
JSValue ResponseRecord(Page& page, const RequestQueue::Request& request, const HttpResponse& response, bool redirected,
                       const char*& error) {
  JSContext* pCtx = page.context();
  const std::string_view origin = page.origin();
  const bool credentials = request.credentials == RequestCredentials::Include;
  // The final URL, after the redirects: valid until the next resolution.
  const std::string_view url = RequestUrl(page.urlResolver(), request.http, {});
  const bool crossOrigin = request.crossOrigin || !IsOfOrigin(url, origin);
  const char* type = "basic";
  if (crossOrigin) {
    switch (request.mode) {
      case RequestMode::SameOrigin:
        error = "redirected to another origin in the same-origin mode";
        return JS_NULL;
      case RequestMode::NoCors: {
        // An opaque response: nothing of it is readable.
        JsArray record(pCtx);
        if (!record.push(JS_NewInt32(pCtx, 0)) || !record.push(NewString(pCtx, {})) ||
            !record.push(NewString(pCtx, {})) || !record.push(JS_FALSE) || !record.push(NewString(pCtx, "opaque")) ||
            !record.push(JS_NewArray(pCtx)) || !record.push(NewArrayBuffer(pCtx, {}))) {
          return JS_EXCEPTION;
        }
        return record.release();
      }
      default:
        if (!CorsCheck(response, origin, credentials)) {
          error = "the response of another origin does not allow the document to read it (CORS)";
          return JS_NULL;
        }
        type = "cors";
        break;
    }
  }

  JsArray record(pCtx);
  if (!record.push(JS_NewInt32(pCtx, response.status())) || !record.push(NewString(pCtx, response.reason())) ||
      !record.push(NewString(pCtx, url)) || !record.push(JS_NewBool(pCtx, redirected)) ||
      !record.push(NewString(pCtx, type))) {
    return JS_EXCEPTION;
  }
  JsArray headers(pCtx);
  const std::string_view exposeHeaders = response.headerValueOrEmpty("access-control-expose-headers");
  for (const auto& [name, value] : response.headers()) {
    if (crossOrigin ? IsExposedResponseHeader(name, exposeHeaders, credentials) : !IsForbiddenResponseHeader(name)) {
      if (!headers.push(NewString(pCtx, name)) || !headers.push(NewString(pCtx, value))) {
        return JS_EXCEPTION;
      }
    }
  }
  if (!record.push(headers.release()) || !record.push(NewArrayBuffer(pCtx, response.bodyInMemory()))) {
    return JS_EXCEPTION;
  }
  return record.release();
}

// Sends 'request' of 'page', after its preflight. Returns its response record (see ResponseRecord()), JS_NULL for a
// network error with its reason in 'error'.
JSValue Send(Page& page, RequestQueue::Request& request, const char*& error) {
  WebSession& session = page.session();
  const std::string_view origin = page.origin();
  const bool credentials = request.credentials == RequestCredentials::Include;
  const http::Method method = request.http.method();
  if (request.preflight) {
    const std::string_view methodName = http::MethodToStr(method);
    // The URL of the request, not sent yet, in the buffer of the URL resolver: copied at once by the preflight.
    HttpRequest preflight =
        session.http
            .makeRequest(origin.size() + methodName.size() + request.corsHeaders.size() + kExtraHeadersSize,
                         http::Method::OPTIONS, RequestUrl(page.urlResolver(), request.http, {}))
            .header(http::Accept, "*/*")
            .headerAddLine("origin", origin)
            .headerAddLine("access-control-request-method", methodName);
    if (!request.corsHeaders.empty()) {
      preflight.headerAddLine("access-control-request-headers", request.corsHeaders);
    }
    const HttpClientResult allowed =
        session.http.requestUpdating(preflight, {.documentUrl = page.url(), .credentials = RequestCredentials::Omit});
    if (!allowed || !PreflightAllows(*allowed, origin, credentials, method, request.corsHeaders)) {
      error = "the CORS preflight of the request to another origin failed";
      return JS_NULL;
    }
  }
  uint32_t redirects = 0;
  const HttpClientResult result = session.http.requestUpdating(
      request.http, {.documentUrl = page.url(), .pRedirects = &redirects, .credentials = request.credentials});
  if (!result) {
    log::debug("[js {}] request {}{} failed: {}", page.url(), request.http.originKey(), request.http.target(),
               ErrcToStr(result.error()));
    error = "Failed to fetch";
    return JS_NULL;
  }
  log::debug("[js {}] request {} {}{}: {}", page.url(), http::MethodToStr(request.http.method()),
             request.http.originKey(), request.http.target(), result->status());
  return ResponseRecord(page, request, *result, redirects != 0, error);
}

JSValue Queue(Page& page, RequestQueue::Request&& request, JSValueConst callback) {
  const int32_t id = page.requests().add(std::move(request), callback);
  return id == 0 ? JS_ThrowOutOfMemory(page.context()) : JS_NewInt32(page.context(), id);
}

JSValue SendNow(Page& page, RequestQueue::Request&& request, JSValueConst /*callback*/) {
  const char* error = nullptr;
  const JSValue record = Send(page, request, error);
  return JS_IsNull(record) ? JS_ThrowDOMException(page.context(), "NetworkError", "%s", error) : record;
}

// request(method, url, headers, body, mode, credentials, callback): queues the request, whose callback receives the
// response record, or null and the reason of a network error. Returns its id, for cancelRequest().
JSValue Request(JSContext* pCtx, JSValueConst /*self*/, [[maybe_unused]] int argc, JSValueConst* argv) {
  assert(argc > kRequestArgCount);
  Page& page = Page::Of(pCtx);
  if (!page.requests().admit(page.session().config.maxRequests)) {
    return JS_ThrowTypeError(pCtx, "aeronet: the scripts of the document exceed WebClientConfig::maxRequests");
  }
  return MakeRequest(pCtx, argv, &Queue, argv[kCallback]);
}

// requestSync(method, url, headers, body, mode, credentials): sends the request at once (synchronous
// XMLHttpRequest) and returns its response record. Throws a NetworkError for a network error.
JSValue RequestSync(JSContext* pCtx, JSValueConst /*self*/, [[maybe_unused]] int argc, JSValueConst* argv) {
  assert(argc >= kRequestArgCount);
  Page& page = Page::Of(pCtx);
  if (!page.requests().admit(page.session().config.maxRequests)) {
    return JS_ThrowDOMException(pCtx, "NetworkError",
                                "aeronet: the scripts of the document exceed WebClientConfig::maxRequests");
  }
  return MakeRequest(pCtx, argv, &SendNow, JS_UNDEFINED);
}

// cancelRequest(id): drops the request 'id', not sent yet: the prelude no longer cancels a request once sent.
JSValue CancelRequest(JSContext* pCtx, JSValueConst /*self*/, [[maybe_unused]] int argc, JSValueConst* argv) {
  assert(argc == 1);
  Page::Of(pCtx).requests().cancel(IntArgument(argv[0]));
  return JS_UNDEFINED;
}

// decode(bytes, contentType): the text of 'bytes' (a Uint8Array), decoded from the encoding of its byte order mark,
// else of the charset of 'contentType', else UTF-8, invalid sequences replaced by U+FFFD.
JSValue Decode(JSContext* pCtx, JSValueConst /*self*/, [[maybe_unused]] int argc, JSValueConst* argv) {
  assert(argc == 2);
  const JsString contentType(pCtx, argv[1]);
  if (contentType.failed()) {
    return JS_EXCEPTION;
  }
  RawChars& buffer = Page::Of(pCtx).textBuffer();
  buffer.clear();
  return NewString(pCtx, DecodeScript(Uint8ArrayView(pCtx, argv[0]), contentType.view(), {}, {}, buffer));
}

// encodeUtf8(text): a Uint8Array of the UTF-8 of 'text', lone surrogates replaced by U+FFFD.
JSValue EncodeUtf8(JSContext* pCtx, JSValueConst /*self*/, int argc, JSValueConst* argv) {
  const JsArgument text(pCtx, argc, argv, 0);
  if (text.failed()) {
    return JS_EXCEPTION;
  }
  return JS_NewUint8ArrayCopy(pCtx, reinterpret_cast<const uint8_t*>(text.view().data()), text.view().size());
}

// parseUrl(input, base): the serialization of the URL parsed from 'input' against 'base' (null for none), '' when it
// is not a valid URL.
JSValue ParseUrl(JSContext* pCtx, JSValueConst /*self*/, int argc, JSValueConst* argv) {
  const JsArgument input(pCtx, argc, argv, 0);
  if (input.failed()) {
    return JS_EXCEPTION;
  }
  if (argc < 2 || JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
    return NewString(pCtx, Page::Of(pCtx).urlResolver().resolve({}, input.view()));
  }
  const JsString base(pCtx, argv[1]);
  if (base.failed()) {
    return JS_EXCEPTION;
  }
  // An invalid base fails the parsing, as does an empty one ("" is not a URL).
  if (base.view().empty()) {
    return NewString(pCtx, {});
  }
  return NewString(pCtx, Page::Of(pCtx).urlResolver().resolve(base.view(), input.view()));
}

// loadNetwork(shared): the object of the globals of the second part of the prelude (NetworkPreludeSource()), run with
// the native helpers (this) and 'shared', the helpers of the first part.
JSValue LoadNetwork(JSContext* pCtx, JSValueConst self, int argc, JSValueConst* argv) {
  WebSession& session = Page::Of(pCtx).session();
  const JsValue function(pCtx,
                         PreludeFunction(pCtx, NetworkPreludeSource(), "aeronet:network", session.networkBytecode,
                                         JsLimits{session.config.maxScriptMemory, session.config.maxScriptStackSize}));
  if (JS_IsException(function.get())) {
    return JS_EXCEPTION;
  }
  JSValueConst args[] = {self, argc > 0 ? argv[0] : JS_UNDEFINED};
  return JS_Call(pCtx, function.get(), JS_UNDEFINED, 2, args);
}

const JSCFunctionListEntry kNetworkNatives[] = {
    JS_CFUNC_DEF("loadNetwork", 1, &JsMethod<LoadNetwork>),
    JS_CFUNC_DEF("request", kRequestArgCount + 1, &JsMethod<Request>),
    JS_CFUNC_DEF("requestSync", kRequestArgCount, &JsMethod<RequestSync>),
    JS_CFUNC_DEF("cancelRequest", 1, &JsMethod<CancelRequest>),
    JS_CFUNC_DEF("decode", 2, &JsMethod<Decode>),
    JS_CFUNC_DEF("encodeUtf8", 1, &JsMethod<EncodeUtf8>),
    JS_CFUNC_DEF("parseUrl", 2, &JsMethod<ParseUrl>),
};

}  // namespace

bool InstallNetwork(JSContext* pCtx, JSValueConst natives) noexcept {
  return SetFunctions(pCtx, natives, kNetworkNatives);
}

bool RunNextRequest(Page& page) {
  JSContext* pCtx = page.context();
  RequestQueue::Request request = page.requests().takeNext();
  const JsValue callback(pCtx, request.callback);
  const char* error = nullptr;
  const JsValue record(pCtx, Send(page, request, error));
  if (JS_IsException(record.get())) {
    return false;
  }
  JsValue reason;
  JSValueConst args[] = {record.get(), JS_UNDEFINED};
  if (JS_IsNull(record.get())) {
    reason = JsValue(pCtx, JS_NewString(pCtx, error));
    if (JS_IsException(reason.get())) {
      return false;
    }
    args[1] = reason.get();
  }
  const JsValue result(pCtx, JS_Call(pCtx, callback.get(), JS_UNDEFINED, 2, args));
  return !JS_IsException(result.get());
}

}  // namespace aeronet::internal
