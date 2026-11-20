#pragma once

#include <quickjs.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/css-selector.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/web-session.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/web-client.hpp"
#include "aeronet/web-page.hpp"
#include "document-url.hpp"
#include "html-node-ops.hpp"
#include "js-engine.hpp"
#include "memory-budget.hpp"
#include "rejection-tracker.hpp"
#include "script-queue.hpp"
#include "timer-queue.hpp"
#include "uncaught-errors.hpp"

struct lxb_dom_node;

namespace aeronet::internal {

// The time 'timeout' after now, the latest time point when that is beyond it (max() means no timeout), now for a
// negative timeout.
[[nodiscard]] SteadyClock::time_point DeadlineIn(std::chrono::milliseconds timeout) noexcept;

// The link of a page to its session, which counts its pages (WebSession::livePages): their WebClient cannot move.
class SessionLink {
 public:
  explicit SessionLink(WebSession& session) noexcept : _session(session) { ++session.livePages; }

  SessionLink(const SessionLink&) = delete;
  SessionLink& operator=(const SessionLink&) = delete;
  SessionLink(SessionLink&&) = delete;
  SessionLink& operator=(SessionLink&&) = delete;

  ~SessionLink() { --_session.livePages; }

  [[nodiscard]] WebSession& session() const noexcept { return _session; }

 private:
  WebSession& _session;
};

// The CSS selector at which the page loop stops (NavigateOptions::untilSelector), checked again only after a change of
// the document.
class SelectorWatch {
 public:
  void watch(const CssSelector* pSelector) noexcept { _pSelector = pSelector; }

  // A script changed the tree or an attribute.
  void touch() noexcept { ++_version; }

  // Whether 'document' changed since the last check, and the selector now matches one of its elements.
  [[nodiscard]] bool changedAndMatches(const HtmlDocument& document);

 private:
  const CssSelector* _pSelector{};
  uint64_t _version{};
  uint64_t _checkedVersion{};
};

// A document of a WebClient with its JavaScript runtime: the bindings of the DOM and of the window (js-dom.cpp,
// js-window.cpp) work on it, and the page loop (page.cpp) runs its scripts.
class Page {
 public:
  enum class ReadyState : uint8_t { Loading, Interactive, Complete };

  // The result of run(): a stop reason, or a navigation to takeNavigation().
  struct Outcome {
    StopReason reason;
    bool navigate;
  };

  // The document of 'url' (http or https, serialized by the URL parser) served with 'status' and the body 'html',
  // parsed with scripting enabled, and the runtime of its scripts, with the Web APIs installed. Throws std::bad_alloc
  // on memory exhaustion.
  Page(WebSession& session, std::string_view url, http::StatusCode status, std::string_view html,
       std::string_view contentType);

  Page(const Page&) = delete;
  Page& operator=(const Page&) = delete;
  Page(Page&&) = delete;
  Page& operator=(Page&&) = delete;

  // Out of line: QuickJS-ng is hidden in aeronet_web, whose code alone frees JavaScript values.
  ~Page();

  // The page of the native functions of a context.
  static Page& Of(JSContext* pCtx) noexcept { return *static_cast<Page*>(JS_GetContextOpaque(pCtx)); }

  // --- Page loop (page.cpp) ---

  // Checks pSelector (can be null), runs the scripts and events of the document, then its timers, until 'until', a
  // match of pSelector, a navigation or 'deadline'.
  Outcome run(const CssSelector* pSelector, LoadState until, SteadyClock::time_point deadline);

  // The URL of the navigation that ended run().
  [[nodiscard]] RawChars32 takeNavigation() noexcept { return std::move(_pendingNavigation); }

  // See WebPage::evaluate().
  std::expected<std::string, std::string> evaluate(std::string_view script, std::chrono::milliseconds timeout);

  // --- State, for WebPage and the bindings ---

  [[nodiscard]] WebSession& session() const noexcept { return _session.session(); }

  [[nodiscard]] JSContext* context() const noexcept { return _engine.context(); }

  [[nodiscard]] HtmlDocument& document() noexcept { return _document; }

  [[nodiscard]] lxb_dom_node* documentNode() const noexcept { return _pDocumentNode; }

  [[nodiscard]] std::string_view url() const noexcept { return _url.view(); }

  // The origin of the URL of the page ("https://example.com:8443"), the key of its storage.
  [[nodiscard]] std::string_view origin() const noexcept { return _url.origin(); }

  [[nodiscard]] std::string_view baseUrl() const noexcept {
    return _baseUrl.empty() ? url() : std::string_view(_baseUrl);
  }

  [[nodiscard]] http::StatusCode status() const noexcept { return _status; }

  [[nodiscard]] StopReason stopReason() const noexcept { return _stopReason; }

  [[nodiscard]] std::span<const std::string_view> uncaughtErrors() const noexcept { return _errors.texts(); }

  [[nodiscard]] ReadyState readyState() const noexcept { return _readyState; }

  [[nodiscard]] lxb_dom_node* currentScript() const noexcept { return _scripts.current(); }

  // The first class of the node wrappers: their classes are kNodeClassCount consecutive ids from it (see js-dom.cpp).
  [[nodiscard]] JSClassID firstNodeClass() const noexcept { return _firstNodeClass; }
  void setFirstNodeClass(JSClassID classId) noexcept { _firstNodeClass = classId; }

  // The wrapper of pNode (null: JS null), created on first use and kept until the page dies (see js-dom.cpp).
  // JS_EXCEPTION on memory exhaustion of the JavaScript heap, std::bad_alloc on that of the C++ code.
  [[nodiscard]] JSValue wrap(lxb_dom_node* pNode);

  // Keeps a reference on 'wrapper', a new wrapper, until the page dies: a node always has the same wrapper.
  void keepWrapper(JSValueConst wrapper) { _wrappers.keep(wrapper); }

  // A script changed the tree or an attribute: the selector of run() is checked again after the task.
  void touchDom() noexcept { _selector.touch(); }

  // Appends 'html', followed by a line feed for 'newLine' (writeln()), to the output of document.write() of the
  // running script, inserted after it when it ends. Ignored outside of a script run by the page loop. Returns false,
  // nothing appended, when the page cannot hold it.
  [[nodiscard]] bool documentWrite(std::string_view html, bool newLine);

  // The URL parsed from 'input' against the base URL, null-terminated, empty when it is not a valid URL. Valid until
  // the next resolution.
  [[nodiscard]] std::string_view resolveUrl(std::string_view input) { return _urlResolver.resolve(baseUrl(), input); }

  // Starts the navigation to 'input', resolved against the base URL, when the current task ends (location setters).
  // A change of the fragment only changes the URL. Returns false, with a SyntaxError thrown in the context, for an
  // invalid URL.
  [[nodiscard]] bool navigateTo(std::string_view input) { return startNavigation(resolveUrl(input), input); }

  // navigateTo() the concatenation of 'inputParts', built without allocation in the buffer of the URL resolver.
  [[nodiscard]] bool navigateTo(std::initializer_list<std::string_view> inputParts);

  // Starts the navigation to the URL of the page when the current task ends (location.reload()).
  void reload() { _pendingNavigation.assign(url()); }

  // Replaces the URL by 'input', resolved against the base URL, without navigation (history.pushState()). Returns
  // false, with an error thrown in the context, for an invalid URL (SyntaxError) or a URL of another origin
  // (SecurityError).
  [[nodiscard]] bool replaceUrl(std::string_view input);

  // The Location object of the page (window.location).
  [[nodiscard]] JSValueConst location() const noexcept { return _location.get(); }
  void setLocation(JsValue location) noexcept { _location = std::move(location); }

  // Records an uncaught error (see WebPage::uncaughtErrors()).
  void reportError(std::string_view text);

  // The timers of the page (setTimeout(), setInterval()).
  [[nodiscard]] TimerQueue& timers() noexcept { return _timers; }

  // The native memory that the scripts make the page hold.
  [[nodiscard]] MemoryBudget& memory() noexcept { return _memory; }

  // The buffer of the bindings that read the document through HtmlElement (textContent, innerHTML...), reused.
  [[nodiscard]] std::string& htmlBuffer() noexcept { return _htmlBuffer; }

  // The area of the origin of the page in 'areas', the localStorage or sessionStorage of its session.
  [[nodiscard]] StorageAreas::Area& storage(StorageAreas& areas) const { return areas.area(origin()); }

 private:
  // A step of the page loop: microtasks, then the stop checks.
  enum class TaskEnd : uint8_t { Continue, Stop, Navigate };

  // A navigation of a <meta http-equiv="refresh">, 'delay' ms after load (-1 for none).
  struct Refresh {
    RawChars32 url;
    int64_t delay;
  };

  void installBindings();

  // Installs the bindings and the prelude. Returns false, the exception pending, on memory or stack exhaustion.
  bool tryInstallBindings();

  // The navigation of navigateTo() to 'url', resolved from 'input' (empty when invalid).
  bool startNavigation(std::string_view url, std::string_view input);

  void runScript(lxb_dom_node* pScript);

  // Makes 'code' (can be the content of _code) the null-terminated content of _code, the code that eval() runs.
  void setCode(std::string_view code);

  // Copies 'filename' (cannot view _code), null-terminated, after the null terminator of the code of _code, whose size
  // does not change: the file name of eval(), valid until the next setCode(). The buffer of the URL resolver would not
  // do: QuickJS reads the file name while it parses, and the script can resolve URLs when it runs. Returns the copy.
  [[nodiscard]] std::string_view setCodeFilename(std::string_view filename);

  // Fetches the script of 'url' whose charset attribute is 'charset' (can be empty) into _code, decoded to UTF-8.
  bool fetchScript(std::string_view url, std::string_view charset);

  void flushDocumentWrite(lxb_dom_node* pScript);

  void reportPendingException(const char* context);

  // Dispatches a trusted event 'type' at pNode, at the window if null.
  void dispatchEvent(lxb_dom_node* pNode, const char* type, bool bubbles);

  // Runs the earliest timer. Returns false for a cleared timer: no task ran.
  bool runNextTimer();

  // Runs the microtasks, then reports the rejected promises left without handler.
  void runMicrotasks();

  TaskEnd endTask();

  TaskEnd runDueTimers();

  [[nodiscard]] Refresh findRefresh();

  [[nodiscard]] Outcome outcome(TaskEnd end) const noexcept { return {_stopReason, end == TaskEnd::Navigate}; }

  static void TrackRejection(JSContext* pCtx, JSValueConst promise, JSValueConst reason, bool isHandled,
                             void* pPage) noexcept;

  SessionLink _session;  // first: the page is counted by its session until it is destroyed
  HtmlDocument _document;
  DocumentUrl _url;
  RawChars32 _baseUrl;  // <base href> resolved against _url, empty without one
  RawChars32 _pendingNavigation;
  UncaughtErrors _errors;
  ScriptQueue _scripts;
  UrlResolver _urlResolver;
  RawChars _code;           // null-terminated at its size, then the null-terminated file name of setCodeFilename()
  std::string _htmlBuffer;  // the buffer type of HtmlElement
  MemoryBudget _memory;
  SelectorWatch _selector;
  lxb_dom_node* _pDocumentNode;
  JSClassID _firstNodeClass{};
  http::StatusCode _status;
  StopReason _stopReason{StopReason::StateReached};
  ReadyState _readyState{ReadyState::Loading};

  // Last: destroyed first, with every JavaScript value, before the document whose nodes the wrappers point to.
  JsEngine _engine;
  JsValues _wrappers{_engine.context()};
  JsValue _location;  // window.location
  JsValue _dispatch;  // the dispatch function of the prelude
  TimerQueue _timers;
  RejectionTracker _rejections{_engine.context()};
};

}  // namespace aeronet::internal
