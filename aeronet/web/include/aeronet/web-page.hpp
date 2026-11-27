#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/chrono-lite.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/http-status-code.hpp"

namespace aeronet {

namespace internal {
class Page;
}  // namespace internal

class WebClient;

// Why WebClient::navigate() returned its page.
enum class StopReason : uint8_t {
  StateReached,     // the page reached the LoadState of NavigateOptions::until
  SelectorMatched,  // an element matched NavigateOptions::untilSelector
  TimedOut,         // NavigateOptions::timeout ran out first: the page is in the state it reached
};

// A page loaded by WebClient::navigate(): its document, as its scripts left it, and its JavaScript runtime, kept for
// evaluate(). It refers to its WebClient (cookie jar, storage): it must not outlive it. Not thread-safe, and the
// thread that uses it must have the free stack of WebClientConfig::maxScriptStackSize.
class WebPage {
 public:
  WebPage(const WebPage&) = delete;
  WebPage& operator=(const WebPage&) = delete;

  // The moved-from page can only be destroyed or assigned to.
  WebPage(WebPage&& other) noexcept;
  WebPage& operator=(WebPage&& other) noexcept;

  ~WebPage();

  // The URL of the document: the one given to navigate(), or the last one reached by redirects and navigations, as
  // changed by history.pushState() and fragment navigations.
  [[nodiscard]] std::string_view url() const noexcept;

  // The status code of the response that served the document: a page is loaded whatever its status, as browsers show
  // error pages.
  [[nodiscard]] http::StatusCode status() const noexcept;

  // The document, with the changes made by its scripts. The HtmlElement handles of a document are not shared with
  // the scripts: modifying the document from C++ is visible to the next scripts.
  [[nodiscard]] const HtmlDocument& document() const noexcept;
  [[nodiscard]] HtmlDocument& document() noexcept;

  [[nodiscard]] StopReason stopReason() const noexcept;

  // The errors that no script caught (thrown exceptions, unhandled promise rejections), as "Error: message" followed
  // by the stack, in the order they happened. Only the first 100 are kept, each cut after 8 KiB. The views are valid as
  // long as the page.
  [[nodiscard]] std::span<const std::string_view> uncaughtErrors() const noexcept;

  // Runs 'script' in the page as a classic script, then its microtasks, and returns its completion value as JSON text
  // (JSON.stringify), empty when the value has no JSON representation (undefined, a function...). Fails with the
  // message of the exception thrown by the script, also when it runs longer than 'timeout' (max() for no time limit).
  // Timers set by the script never run, and the navigations it starts are ignored.
  std::expected<std::string, std::string> evaluate(std::string_view script,
                                                   std::chrono::milliseconds timeout = std::chrono::seconds{10});

 private:
  friend class WebClient;

  explicit WebPage(std::unique_ptr<internal::Page> pPage) noexcept;

  std::unique_ptr<internal::Page> _pPage;
};

}  // namespace aeronet
