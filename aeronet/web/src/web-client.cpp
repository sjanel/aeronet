#include "aeronet/web-client.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/client-connection.hpp"
#include "aeronet/css-selector.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/web-session.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/web-client-config.hpp"
#include "aeronet/web-page.hpp"
#include "html-node-ops.hpp"
#include "page.hpp"
#include "web-url.hpp"

namespace aeronet {

namespace {

// The Accept header of the navigation requests of browsers.
constexpr std::string_view kDocumentAccept = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";

// The URL of the last request sent for 'request' (after the redirects followed) with the fragment of 'requestedUrl'
// (never sent), as browsers keep it, serialized by 'resolver': valid until its next resolution.
std::string_view FinalUrl(internal::UrlResolver& resolver, const HttpRequest& request, std::string_view requestedUrl) {
  const std::string_view target = request.target();
  assert(!target.contains('#'));  // the HTTP client strips the fragments, of the redirects too
  const std::size_t hash = requestedUrl.find('#');
  const std::string_view fragment = hash == std::string_view::npos ? std::string_view{} : requestedUrl.substr(hash);
  if (target.starts_with('/') || target.starts_with('?')) {
    return resolver.resolve({}, {request.scheme(), "://", request.hostHeaderValue(), target, fragment});
  }
  return resolver.resolve({}, {target, fragment});  // absolute-form, sent to a forward proxy
}

std::expected<std::unique_ptr<internal::Page>, WebErrc> LoadPage(internal::WebSession& session,
                                                                 internal::UrlResolver& resolver,
                                                                 std::string_view url) {
  // A valid http URL that the HTTP client cannot request: with user info, port 0, a too long target...
  if (!HttpRequest::IsValidUrl(url)) {
    log::warn("Document {} not requested: the HTTP client does not support its URL", url);
    return std::unexpected(WebErrc::invalidUrl);
  }
  HttpRequest request = session.http.makeRequest(http::Method::GET, url).header(http::Accept, kDocumentAccept);
  const HttpClientResult result = session.http.requestUpdating(request);
  if (!result) {
    log::warn("Request of the document {} failed: {}", url, ErrcToStr(result.error()));
    return std::unexpected(WebErrc::requestFailed);
  }
  return std::make_unique<internal::Page>(session, FinalUrl(resolver, request, url), result->status(),
                                          result->bodyInMemory(), result->headerValueOrEmpty(http::ContentType));
}

WebClientConfig Validated(WebClientConfig config) {
  config.validate();
  return config;
}

// Throws std::logic_error if 'session' has pages: they point to it.
void CheckNoPages(const internal::WebSession& session) {
  if (session.livePages != 0) {
    throw std::logic_error("A WebClient cannot move, nor be assigned to, while it has pages (" +
                           std::to_string(session.livePages) + "): they point to it");
  }
}

internal::WebSession&& Movable(internal::WebSession& session) {
  CheckNoPages(session);
  return std::move(session);
}

}  // namespace

void WebClientConfig::validate() const {
  http.validate();
  if (maxScriptMemory == 0) {
    throw std::invalid_argument("WebClientConfig::maxScriptMemory must not be 0 (max() for no limit)");
  }
  if (maxScriptStackSize == 0) {
    throw std::invalid_argument("WebClientConfig::maxScriptStackSize must not be 0 (max() for no limit)");
  }
  if (maxTimerDelay < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("WebClientConfig::maxTimerDelay must not be negative");
  }
}

WebClient::WebClient() : WebClient(WebClientConfig{}) {}

WebClient::WebClient(WebClientConfig config) : _session(Validated(std::move(config))) {}

// The moves throw while pages point to the client (see the header).
// NOLINTNEXTLINE(performance-noexcept-move-constructor,bugprone-exception-escape)
WebClient::WebClient(WebClient&& other) : _session(Movable(other._session)) {}

// NOLINTNEXTLINE(performance-noexcept-move-constructor,bugprone-exception-escape)
WebClient& WebClient::operator=(WebClient&& other) {
  if (this != &other) {
    CheckNoPages(_session);
    _session = Movable(other._session);
  }
  return *this;
}

WebClient::~WebClient() = default;

std::expected<WebPage, WebErrc> WebClient::navigate(std::string_view url, const NavigateOptions& options) {
  const CssSelector selector = options.untilSelector.empty() ? CssSelector() : CssSelector(options.untilSelector);
  const SteadyClock::time_point deadline = internal::DeadlineIn(options.timeout);
  internal::UrlResolver resolver;
  RawChars32 target(resolver.resolve({}, url));
  if (!internal::IsHttpUrl(target)) {
    return std::unexpected(WebErrc::invalidUrl);
  }
  for (uint32_t navigations = 0;; ++navigations) {
    std::expected<std::unique_ptr<internal::Page>, WebErrc> pPage = LoadPage(_session, resolver, target);
    if (!pPage) {
      return std::unexpected(pPage.error());
    }
    internal::Page& page = **pPage;
    const internal::Page::Outcome outcome = page.run(selector.empty() ? nullptr : &selector, options.until, deadline);
    if (!outcome.navigate) {
      return WebPage(std::move(*pPage));
    }
    if (navigations == _session.config.maxNavigations) {
      log::warn("Navigation from {} stopped: more than {} navigations", page.url(), _session.config.maxNavigations);
      return std::unexpected(WebErrc::tooManyNavigations);
    }
    target = page.takeNavigation();
    log::debug("Navigation from {} to {}", page.url(), std::string_view(target));
  }
}

WebPage::WebPage(std::unique_ptr<internal::Page> pPage) noexcept : _pPage(std::move(pPage)) {}

WebPage::WebPage(WebPage&& other) noexcept = default;
WebPage& WebPage::operator=(WebPage&& other) noexcept = default;
WebPage::~WebPage() = default;

std::string_view WebPage::url() const noexcept { return _pPage->url(); }

http::StatusCode WebPage::status() const noexcept { return _pPage->status(); }

const HtmlDocument& WebPage::document() const noexcept { return _pPage->document(); }

HtmlDocument& WebPage::document() noexcept { return _pPage->document(); }

StopReason WebPage::stopReason() const noexcept { return _pPage->stopReason(); }

std::span<const std::string_view> WebPage::uncaughtErrors() const noexcept { return _pPage->uncaughtErrors(); }

std::expected<std::string, std::string> WebPage::evaluate(std::string_view script, std::chrono::milliseconds timeout) {
  return _pPage->evaluate(script, timeout);
}

}  // namespace aeronet
