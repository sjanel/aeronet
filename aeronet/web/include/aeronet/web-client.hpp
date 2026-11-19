#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string_view>

#include "aeronet/http-client.hpp"
#include "aeronet/internal/web-session.hpp"
#include "aeronet/web-client-config.hpp"
#include "aeronet/web-page.hpp"

namespace aeronet {

// The loading step at which WebClient::navigate() returns its page, if NavigateOptions::untilSelector does not match
// before. The scripts of a document run once it is parsed, in the order of a browser: parser-blocking scripts, defer
// scripts, DOMContentLoaded, async scripts, load. Timers do not wait: those due at once (setTimeout(fn) during the
// loading) run between these tasks, the others when the virtual clock advances, after load.
enum class LoadState : uint8_t {
  DomContentLoaded,  // the parser-blocking and defer scripts have run, DOMContentLoaded was dispatched
  Load,              // then the async scripts, and the load event: a loaded page for a browser
  NetworkIdle,       // then the timers, until none is left within WebClientConfig::maxTimerDelay
};

struct NavigateOptions {
  // A CSS selector: navigate() returns as soon as an element matches it, checked when a document is parsed, before
  // its scripts run (data served in the HTML costs no script), then after each script, event and timer that changed
  // the document. Empty: the page loads until 'until' or 'timeout'. navigate() throws std::invalid_argument if it is
  // not a valid selector.
  // Without {}, designated initializers that omit it ({.until = ...}) warn about a missing initializer.
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::string_view untilSelector{};

  // The loading step at which navigate() returns, unless untilSelector matches first.
  LoadState until{LoadState::Load};

  // Time budget of the whole navigation (requests and scripts of every document reached): a running script is
  // stopped, and the page returned as it is. A request already sent can exceed it by up to the requestTimeout of
  // WebClientConfig::http. std::chrono::milliseconds::max() means no time limit.
  std::chrono::milliseconds timeout{std::chrono::seconds{10}};
};

enum class WebErrc : uint8_t {
  invalidUrl,          // the URL given to navigate(), or of a navigation of a page, is not an http or https URL that
                       // the HTTP client can request (with user info or port 0, for instance)
  requestFailed,       // the request of a document failed (connection, TLS, timeout...: logged with its cause)
  tooManyNavigations,  // the pages started more navigations than WebClientConfig::maxNavigations
};

[[nodiscard]] constexpr std::string_view ErrcToStr(WebErrc errc) noexcept {
  switch (errc) {
    case WebErrc::invalidUrl:
      return "invalid http or https URL";
    case WebErrc::requestFailed:
      return "request of the document failed";
    default:
      return "too many navigations";
  }
}

// A client that loads web pages as a browser without rendering: it fetches a document, parses it, and runs its scripts
// (QuickJS-ng) over a DOM of the document, with cookies, storage, timers on a virtual clock, location and history.
// See docs/modules/web.md for what the scripts can use.
//
//   WebClient web;
//   auto page = web.navigate(url, {.untilSelector = "table.fees tr"});
//   if (page) {
//     for (HtmlElement row : page->document().querySelectorAll("table.fees tr")) { ... }
//   }
//
// The cookies (and the localStorage of each origin) live as long as the client, across navigate() calls. Its pages
// point to it: it must outlive them, and cannot move while it has some.
// The scripts run in the process: they are bounded in memory, stack and time (WebClientConfig, NavigateOptions), but a
// bug of the engine is exploitable by a hostile page. Load untrusted sites from an unprivileged process.
// Not thread-safe.
class WebClient {
 public:
  // A client with the default configuration.
  WebClient();

  // Throws std::invalid_argument if 'config' is invalid (see WebClientConfig::validate()).
  explicit WebClient(WebClientConfig config);

  WebClient(const WebClient&) = delete;
  WebClient& operator=(const WebClient&) = delete;

  // Throws std::logic_error if 'other', or this client for an assignment, has pages: they point to it. The moved-from
  // client can only be destroyed or assigned to.
  WebClient(WebClient&& other);             // NOLINT(performance-noexcept-move-constructor)
  WebClient& operator=(WebClient&& other);  // NOLINT(performance-noexcept-move-constructor)

  ~WebClient();

  // Loads the page of 'url' (an http or https URL) and runs its scripts until 'options' says to stop, following the
  // navigations of its scripts. Fails when the URL is invalid, when the request of a document fails, or after too many
  // navigations; a page whose scripts throw is still returned (see WebPage::uncaughtErrors()).
  // Throws std::invalid_argument if options.untilSelector is not a valid CSS selector, std::bad_alloc on memory
  // exhaustion, std::runtime_error when WebClientConfig::maxScriptMemory is too small to set up the runtime of a page.
  [[nodiscard]] std::expected<WebPage, WebErrc> navigate(std::string_view url, const NavigateOptions& options = {});

  // The HTTP client of the pages, whose cookie jar holds the cookies of the visited sites.
  [[nodiscard]] HttpClient& httpClient() noexcept { return _session.http; }

  [[nodiscard]] const WebClientConfig& config() const noexcept { return _session.config; }

 private:
  internal::WebSession _session;
};

}  // namespace aeronet
