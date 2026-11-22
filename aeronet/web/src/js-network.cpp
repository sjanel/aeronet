// The requests of the scripts (fetch(), XMLHttpRequest, navigator.sendBeacon()) over the HTTP client of the page, with
// the rules of the Fetch Standard (forbidden headers, CORS, credentials, Referer), and the text and URL helpers of the
// prelude.

#include "js-network.hpp"

#include <quickjs.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

#include "aeronet/client-connection.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-message-common.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/internal/response-streamer.hpp"
#include "aeronet/internal/web-session.hpp"
#include "aeronet/log.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/web-client-config.hpp"
#include "cors.hpp"
#include "html-node-ops.hpp"
#include "js-engine.hpp"
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

// Whether a request of 'mode' sends an Origin (Fetch "HTTP-network-or-cache fetch"): with the response tainting 'cors'
// (its URL, or one of its redirects, of another origin in the cors mode), or a method other than GET and HEAD.
constexpr bool SendsOrigin(RequestMode mode, bool crossOrigin, http::Method method) noexcept {
  return (crossOrigin && mode == RequestMode::Cors) || (method != http::Method::GET && method != http::Method::HEAD);
}

// Whether a request of 'mode' needs a CORS preflight (Fetch "main fetch"): of the cors mode, to another origin or
// redirected through one, with a method or headers ('corsHeaders', the unsafe ones) that a form could not send.
constexpr bool NeedsPreflight(RequestMode mode, bool crossOrigin, http::Method method,
                              std::string_view corsHeaders) noexcept {
  return mode == RequestMode::Cors && crossOrigin && (!IsCorsSafelistedMethod(method) || !corsHeaders.empty());
}

// Adds to 'http' the Origin 'origin' when 'sendsOrigin', and the Referer 'referrer' unless empty.
void AddOriginAndReferer(HttpRequest& http, bool sendsOrigin, std::string_view origin, std::string_view referrer) {
  if (sendsOrigin) {
    http.headerAddLine("origin", origin);
  }
  if (!referrer.empty()) {
    http.headerAddLine("referer", referrer);
  }
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

// The prelude gives the arguments of the natives, but it reads them from objects that page scripts can replace: the
// getters of Request.prototype, Array.prototype.join(), the global Uint8Array... The natives check what they cannot
// convert, and convert all their arguments before they keep anything from the page: a conversion can run a script
// (toString()), which can resolve another URL or detach the buffer of a Uint8Array.

// Sets 'bytes' to the elements of 'array', a non-empty Uint8Array given by the prelude. Returns false, with a TypeError
// thrown, for another object or a Uint8Array whose buffer is detached (a QuickJS-ng buffer has a byte at least, so
// JS_GetUint8Array() returns null only on failure).
[[nodiscard]] bool Uint8ArrayView(JSContext* pCtx, JSValueConst array, std::string_view& bytes) noexcept {
  std::size_t size = 0;
  const uint8_t* pData = JS_GetUint8Array(pCtx, &size, array);
  bytes = {reinterpret_cast<const char*>(pData), size};
  return pData != nullptr;
}

// An id given by the prelude, from its own variables.
int32_t IdArgument(JSValueConst value) noexcept {
  assert(JS_VALUE_GET_TAG(value) == JS_TAG_INT);
  return JS_VALUE_GET_INT(value);
}

// The enumerators of RequestMode and RequestCredentials, given by the prelude as int32 values.
constexpr uint32_t kRequestModes = static_cast<uint32_t>(RequestMode::SameOrigin) + 1U;
constexpr uint32_t kRequestCredentials = static_cast<uint32_t>(RequestCredentials::Omit) + 1U;

// The enumerator 'value' below 'count', read by the prelude from the getters of a Request. -1, with a TypeError thrown,
// for another value.
int32_t EnumArgument(JSContext* pCtx, JSValueConst value, uint32_t count) noexcept {
  if (JS_VALUE_GET_TAG(value) == JS_TAG_INT) {
    const auto index = static_cast<uint32_t>(JS_VALUE_GET_INT(value));  // a negative value is above count
    if (index < count) {
      return static_cast<int32_t>(index);
    }
  }
  JS_ThrowTypeError(pCtx, "aeronet: invalid mode or credentials of a request");
  return -1;
}

// The bytes of the body argument of request(): none for null, the UTF-8 of a string, the elements of a non-empty
// Uint8Array (the prelude converts the other bodies). A local object, like JsString.
class BodyBytes {
 public:
  BodyBytes(JSContext* pCtx, JSValueConst body) noexcept {
    if (JS_IsString(body)) {
      _pCtx = pCtx;
      _pData = ToUtf8(pCtx, body, _size);  // a rope too
      _size = _pData == nullptr ? kFailed : _size;
    } else if (!JS_IsNull(body)) {
      std::string_view bytes;
      _size = Uint8ArrayView(pCtx, body, bytes) ? bytes.size() : kFailed;
      _pData = bytes.data();
    }
  }

  BodyBytes(const BodyBytes&) = delete;
  BodyBytes& operator=(const BodyBytes&) = delete;
  BodyBytes(BodyBytes&&) = delete;
  BodyBytes& operator=(BodyBytes&&) = delete;

  ~BodyBytes() {
    if (_pCtx != nullptr) {
      JS_FreeCString(_pCtx, _pData);  // nothing for a null pointer
    }
  }

  // Whether the conversion failed (memory exhaustion, not a Uint8Array), its exception pending.
  [[nodiscard]] bool failed() const noexcept { return _size == kFailed; }

  // Not for a failed conversion.
  [[nodiscard]] std::string_view view() const noexcept {
    assert(!failed());
    return {_pData, _size};
  }

 private:
  static constexpr std::size_t kFailed = std::numeric_limits<std::size_t>::max();

  JSContext* _pCtx{};  // set for a string body: its C string is freed with this object
  const char* _pData{};
  std::size_t _size{};
};

// What request() and requestSync() do with the request of their arguments: queue it with its callback, or send it.
using RequestUse = JSValue (*)(Page& page, RequestQueue::Request&& request, JSValueConst callback);

// The request of the arguments of request() or requestSync(), given to use(page, request, callback) whose result is
// returned. JS_EXCEPTION, with a TypeError thrown, for a request that fetch() refuses: an unsupported method or URL, a
// request to another origin in the same-origin mode, an unsafe one in the no-cors mode, a body with GET or HEAD, an
// invalid header.
JSValue MakeRequest(JSContext* pCtx, JSValueConst* argv, RequestUse use, JSValueConst callback) {
  const JsString methodArg(pCtx, argv[kMethod]);
  if (methodArg.failed()) {
    return JS_EXCEPTION;
  }
  const JsString urlArg(pCtx, argv[kUrl]);
  if (urlArg.failed()) {
    return JS_EXCEPTION;
  }
  const JsString lines(pCtx, argv[kHeaders]);
  if (lines.failed()) {
    return JS_EXCEPTION;
  }
  const BodyBytes bodyBytes(pCtx, argv[kBody]);  // runs no script
  if (bodyBytes.failed()) {
    return JS_EXCEPTION;
  }
  const int32_t mode = EnumArgument(pCtx, argv[kMode], kRequestModes);
  if (mode < 0) {
    return JS_EXCEPTION;
  }
  const int32_t credentials = EnumArgument(pCtx, argv[kCredentials], kRequestCredentials);
  if (credentials < 0) {
    return JS_EXCEPTION;
  }

  const http::Method method = http::ParseMethodStr(methodArg.view());
  if (method == http::kMethodInvalid || method == http::Method::CONNECT || method == http::Method::TRACE) {
    return JS_ThrowTypeError(pCtx, "aeronet: the HTTP method '%.*s' is not supported",
                             static_cast<int>(methodArg.view().size()), methodArg.view().data());
  }
  Page& page = Page::Of(pCtx);
  // In the buffer of the URL resolver, until the next resolution: none before the request copies it. Absolute, as
  // resolved by the prelude (Request, XMLHttpRequest.open()).
  const std::string_view url = WithoutFragment(page.resolveUrl(urlArg.view()));
  if (!IsHttpUrl(url) || !HttpRequest::IsValidUrl(url)) {
    return JS_ThrowTypeError(pCtx, "aeronet: only http and https URLs can be fetched, not '%.*s'",
                             static_cast<int>(url.size()), url.data());
  }
  const std::string_view origin = page.origin();
  const bool crossOrigin = !IsOfOrigin(url, origin);
  const auto requestMode = static_cast<RequestMode>(mode);
  if (requestMode == RequestMode::NoCors && !IsCorsSafelistedMethod(method)) {
    return JS_ThrowTypeError(pCtx, "aeronet: the method '%.*s' is not allowed in the no-cors mode",
                             static_cast<int>(methodArg.view().size()), methodArg.view().data());
  }

  if (requestMode == RequestMode::SameOrigin && crossOrigin) {
    return JS_ThrowTypeError(pCtx,
                             "Failed to fetch '%.*s': its origin is not that of the document in the "
                             "same-origin mode",
                             static_cast<int>(url.size()), url.data());
  }

  const std::string_view body = bodyBytes.view();
  // Refused by the Request constructor, dropped by XMLHttpRequest.send(), unless a page script replaced what the
  // prelude reads the method from.
  if (!body.empty() && (method == http::Method::GET || method == http::Method::HEAD)) {
    return JS_ThrowTypeError(pCtx, "aeronet: a %.*s request has no body", static_cast<int>(methodArg.view().size()),
                             methodArg.view().data());
  }
  const std::string_view referrer = ReferrerOf(page.url(), origin, url);
  // The lines become headers of about their size, with room for Accept, Origin, Referer and the body headers: no
  // reallocation of the request.
  const std::size_t capacity = lines.view().size() + referrer.size() + origin.size() + kExtraHeadersSize + body.size();

  RequestQueue::Request request{
      page.session().http.makeRequest(capacity, method, url),
      {},
      JS_UNDEFINED,
      kRequestMemory + url.size() + capacity,
      requestMode,
      static_cast<RequestCredentials>(credentials),
      crossOrigin,
      false,
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
  request.preflight = NeedsPreflight(requestMode, crossOrigin, method, request.corsHeaders);
  if (!accept) {
    request.http.header(http::Accept, "*/*");
  }

  AddOriginAndReferer(request.http, SendsOrigin(requestMode, crossOrigin, method), origin, referrer);

  if (!body.empty()) {
    request.http.body(body, contentType.empty() ? http::ContentTypeApplicationOctetStream : contentType);
  }
  return use(page, std::move(request), callback);
}

// The state of the redirect chain of a request of a script while it is sent (Fetch "HTTP-redirect fetch").
struct RedirectChain {
  // The origin of the request for the CORS protocol: that of the document, "null" once tainted.
  [[nodiscard]] std::string_view requestOrigin() const noexcept {
    return taintedOrigin ? std::string_view("null") : pPage->origin();
  }

  Page* pPage;
  RequestQueue::Request* pRequest;
  const char* error{};  // the reason of the network error that stopped the chain, null if none
  uint32_t redirects{};
  bool crossOriginUrl;   // the current URL is of another origin than the document
  bool crossSite{};      // a redirect changed the site
  bool taintedOrigin{};  // redirected from another origin to a third one: its Origin is "null"
};

// The rules of the Fetch Standard for a redirect of a request of a script (see PageRequestContext::RedirectHook), with
// its RedirectChain as 'pData': the redirect response of another origin needs the consent of its server, the
// same-origin mode fails for another origin, the Origin becomes "null" after a redirect from another origin to a third
// one, the Referer follows the new URL, and a request that needs a preflight at its new URL stops for it.
bool OnRedirect(void* pData, HttpRequest& req, const HttpResponse& redirect, HttpRequest::RedirectOutcome outcome) {
  RedirectChain& chain = *static_cast<RedirectChain*>(pData);
  Page& page = *chain.pPage;
  RequestQueue::Request& request = *chain.pRequest;
  const std::string_view origin = page.origin();

  // The HTTP client bounds the redirects of each call of requestUpdating(): those of a chain continued after a
  // preflight count all together.
  if (++chain.redirects > page.session().config.http.maxRedirects) {
    chain.error = "too many redirects";
    return false;
  }

  if (request.mode == RequestMode::Cors && request.crossOrigin &&
      !CorsCheck(redirect, chain.requestOrigin(), request.credentials == RequestCredentials::Include)) {
    chain.error = "the redirect of another origin does not allow the document to follow it (CORS)";
    return false;
  }

  // Valid until the next resolution.
  const std::string_view url = RequestUrl(page.urlResolver(), req, {});
  const bool crossOriginUrl = !IsOfOrigin(url, origin);
  if (crossOriginUrl && request.mode == RequestMode::SameOrigin) {
    chain.error = "redirected to another origin in the same-origin mode";
    return false;
  }

  chain.taintedOrigin =
      chain.taintedOrigin || (outcome != HttpRequest::RedirectOutcome::SameOrigin && chain.crossOriginUrl);
  chain.crossSite = chain.crossSite || outcome == HttpRequest::RedirectOutcome::CrossSite;
  chain.crossOriginUrl = crossOriginUrl;
  request.crossOrigin = request.crossOrigin || crossOriginUrl;
  req.headerRemoveLine("origin").headerRemoveLine("referer");
  AddOriginAndReferer(req, SendsOrigin(request.mode, request.crossOrigin, req.method()), chain.requestOrigin(),
                      ReferrerOf(page.url(), origin, url));
  request.preflight = NeedsPreflight(request.mode, request.crossOrigin, req.method(), request.corsHeaders);
  return !request.preflight;
}

// A preflight is never redirected (Fetch "CORS-preflight fetch"): its redirect fails it, as not a success.
bool StopAtRedirect(void* /*pData*/, HttpRequest& /*req*/, const HttpResponse& /*redirect*/,
                    HttpRequest::RedirectOutcome /*outcome*/) noexcept {
  return false;
}

// Sends the CORS preflight of 'request' of 'page' to its current URL, with the Origin 'origin'. Returns whether its
// response allows the request.
bool Preflight(Page& page, const RequestQueue::Request& request, std::string_view origin) {
  WebSession& session = page.session();
  const http::Method method = request.http.method();
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

  const HttpClientResult allowed = session.http.requestUpdating(
      preflight, {.documentUrl = page.url(), .onRedirect = &StopAtRedirect, .credentials = RequestCredentials::Omit});
  return allowed && PreflightAllows(*allowed, origin, request.credentials == RequestCredentials::Include, method,
                                    request.corsHeaders);
}

// The response of the scripts for 'response', the final response of 'request' sent by 'page' through 'chain': [status,
// statusText, url, redirected, type, headers (names and values in turn), body (ArrayBuffer)], or JS_NULL for a network
// error with its reason in 'error'. JS_EXCEPTION on memory exhaustion.
JSValue ResponseRecord(Page& page, const RequestQueue::Request& request, const RedirectChain& chain,
                       const HttpResponse& response, const char*& error) {
  JSContext* pCtx = page.context();
  const bool credentials = request.credentials == RequestCredentials::Include;
  const char* type = "basic";
  if (request.crossOrigin) {
    // The same-origin mode fails before a request to another origin (MakeRequest() and OnRedirect()).
    assert(request.mode != RequestMode::SameOrigin);
    if (request.mode == RequestMode::NoCors) {
      // An opaque response: nothing of it is readable.
      JsArray record(pCtx);
      record.append({
          JS_NewInt32(pCtx, 0),
          NewString(pCtx, {}),
          NewString(pCtx, {}),
          JS_FALSE,
          NewString(pCtx, "opaque"),
          JS_NewArray(pCtx),
          NewArrayBuffer(pCtx, {}),
      });
      return record.release();
    }
    if (!CorsCheck(response, chain.requestOrigin(), credentials)) {
      error = "the response of another origin does not allow the document to read it (CORS)";
      return JS_NULL;
    }
    type = "cors";
  }

  // On memory exhaustion, the next values are still created, and freed: an error path, the record is small.
  JsArray headers(pCtx);
  const std::string_view exposeHeaders = response.headerValueOrEmpty("access-control-expose-headers");
  for (const auto& [name, value] : response.headers()) {
    if (request.crossOrigin ? !IsExposedResponseHeader(name, exposeHeaders, credentials)
                            : IsForbiddenResponseHeader(name)) {
      continue;  // not readable by the document
    }
    headers.append({NewString(pCtx, name), NewString(pCtx, value)});
  }
  JsArray record(pCtx);
  // The final URL, after the redirects, is valid until the next resolution: copied at once.
  record.append({
      JS_NewInt32(pCtx, response.status()),
      NewString(pCtx, response.reason()),
      NewString(pCtx, RequestUrl(page.urlResolver(), request.http, {})),
      JS_NewBool(pCtx, chain.redirects != 0),
      NewString(pCtx, type),
      headers.release(),
      NewArrayBuffer(pCtx, response.bodyInMemory()),
  });
  return record.release();
}

// Sends 'request' of 'page', with the preflights it needs, at its URL and after a redirect to another origin.
// Returns its response record (see ResponseRecord()), JS_NULL for a network error with its reason in 'error'.
JSValue Send(Page& page, RequestQueue::Request& request, const char*& error) {
  WebSession& session = page.session();
  RedirectChain chain{.pPage = &page, .pRequest = &request, .crossOriginUrl = request.crossOrigin};
  for (;;) {
    if (request.preflight) {
      if (!Preflight(page, request, chain.requestOrigin())) {
        error = "the CORS preflight of the request to another origin failed";
        return JS_NULL;
      }
      request.preflight = false;
    }
    const PageRequestContext context{
        .documentUrl = page.url(),
        .onRedirect = &OnRedirect,
        .pRedirectData = &chain,
        .credentials = request.credentials,
        .crossSiteChain = chain.crossSite,
        .crossOriginChain = request.crossOrigin,
    };
    const HttpClientResult result = session.http.requestUpdating(request.http, context);
    if (!result) {
      log::debug("[js {}] request {}{} failed: {}", page.url(), request.http.originKey(), request.http.target(),
                 ErrcToStr(result.error()));
      error = "Failed to fetch";
      return JS_NULL;
    }
    if (chain.error != nullptr) {
      error = chain.error;
      return JS_NULL;
    }
    if (!request.preflight) {
      log::debug("[js {}] request {} {}{}: {}", page.url(), http::MethodToStr(request.http.method()),
                 request.http.originKey(), request.http.target(), result->status());
      // A redirect that the HTTP client did not follow: beyond its maxRedirects, or from https to http.
      if (session.config.http.followRedirects && IsRedirectStatus(result->status()) &&
          !result->headerValueOrEmpty(http::Location).empty()) {
        error = "the redirect was not followed (too many redirects, or from https to http)";
        return JS_NULL;
      }
      return ResponseRecord(page, request, chain, *result, error);
    }
    // Stopped by OnRedirect() before a URL that needs a preflight: continued after it.
  }
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

// cancelRequest(id): drops the request 'id' if not sent yet.
JSValue CancelRequest(JSContext* pCtx, JSValueConst /*self*/, [[maybe_unused]] int argc, JSValueConst* argv) {
  assert(argc == 1);
  Page::Of(pCtx).requests().cancel(IdArgument(argv[0]));
  return JS_UNDEFINED;
}

// decode(bytes, contentType): the text of 'bytes' (a non-empty Uint8Array), decoded from the encoding of its byte order
// mark, else of the charset of 'contentType', else UTF-8, invalid sequences replaced by U+FFFD (XMLHttpRequest).
JSValue Decode(JSContext* pCtx, JSValueConst /*self*/, [[maybe_unused]] int argc, JSValueConst* argv) {
  assert(argc == 2);
  const JsString contentType(pCtx, argv[1]);
  std::string_view bytes;
  if (contentType.failed() || !Uint8ArrayView(pCtx, argv[0], bytes)) {
    return JS_EXCEPTION;
  }
  return NewString(pCtx, DecodeScript(bytes, contentType.view(), {}, {}, Page::Of(pCtx).textBuffer()));
}

// decodeUtf8(bytes, fatal): the text of 'bytes' (a non-empty Uint8Array) decoded as UTF-8, a byte order mark included,
// invalid sequences replaced by U+FFFD, or null for one when 'fatal' is true (TextDecoder, the bodies of fetch()).
JSValue DecodeUtf8Bytes(JSContext* pCtx, JSValueConst /*self*/, [[maybe_unused]] int argc, JSValueConst* argv) {
  assert(argc == 2);
  std::string_view bytes;
  if (!Uint8ArrayView(pCtx, argv[0], bytes)) {
    return JS_EXCEPTION;
  }
  RawChars& buffer = Page::Of(pCtx).textBuffer();
  const std::string_view text = DecodeUtf8(bytes, buffer);
  // DecodeUtf8() returns 'bytes' itself exactly when they are valid.
  if (text.data() != bytes.data() && JS_ToBool(pCtx, argv[1]) != 0) {
    return JS_NULL;
  }
  return NewString(pCtx, text);
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
JSValue ParseUrl(JSContext* pCtx, JSValueConst /*self*/, [[maybe_unused]] int argc, JSValueConst* argv) {
  assert(argc == 2);
  const JsString input(pCtx, argv[0]);
  if (input.failed()) {
    return JS_EXCEPTION;
  }

  if (JS_IsNull(argv[1])) {
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

const JSCFunctionListEntry kNetworkNatives[]{
    JS_CFUNC_DEF("request", kRequestArgCount + 1, &JsMethod<Request>),
    JS_CFUNC_DEF("requestSync", kRequestArgCount, &JsMethod<RequestSync>),
    JS_CFUNC_DEF("cancelRequest", 1, &JsMethod<CancelRequest>),
    JS_CFUNC_DEF("decode", 2, &JsMethod<Decode>),
    JS_CFUNC_DEF("decodeUtf8", 2, &JsMethod<DecodeUtf8Bytes>),
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

  JsValue record(pCtx, Send(page, request, error));
  if (JS_IsException(record.get())) {
    // The JavaScript heap cannot hold the response: a network error for the script. Its callback runs, so that the
    // prelude forgets the request.
    JS_FreeValue(pCtx, JS_GetException(pCtx));
    record = JsValue(pCtx, JS_NULL);
    error = "the response does not fit in the memory of the scripts (WebClientConfig::maxScriptMemory)";
  }

  JsValue reason;
  JSValueConst args[]{record.get(), JS_UNDEFINED};

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
