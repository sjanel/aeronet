#pragma once

#include <quickjs.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/address-space.hpp"
#include "aeronet/css-selector.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/html-element.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/web-session.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/web-client.hpp"
#include "aeronet/web-page.hpp"
#include "document-url.hpp"
#include "html-node-ops.hpp"
#include "js-dom.hpp"
#include "js-engine.hpp"
#include "js-prelude.hpp"
#include "memory-budget.hpp"
#include "module-map.hpp"
#include "rejection-tracker.hpp"
#include "request-queue.hpp"
#include "script-queue.hpp"
#include "timer-queue.hpp"
#include "uncaught-errors.hpp"

struct lxb_dom_node;

namespace aeronet::internal {

// The time 'timeout' after now, the latest time point when that is beyond it (max() means no timeout), now for a
// negative timeout.
[[nodiscard]] SteadyClock::time_point DeadlineIn(std::chrono::milliseconds timeout) noexcept;

// The URL of the last request sent for 'request' (after the redirects followed), with 'fragment' (never sent, empty or
// starting with '#'), serialized by 'resolver': valid until its next resolution.
[[nodiscard]] std::string_view RequestUrl(UrlResolver& resolver, const HttpRequest& request, std::string_view fragment);

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

  // The document of 'url' (http or https, serialized by the URL parser) served with 'status' and the body 'html' by a
  // server of 'addressSpace', parsed with scripting enabled, and the runtime of its scripts, with the Web APIs
  // installed. Throws std::bad_alloc on memory exhaustion.
  Page(WebSession& session, std::string_view url, http::StatusCode status, std::string_view html,
       std::string_view contentType, AddressSpace addressSpace);

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

  // url(), null-terminated: the file name of the inline scripts.
  [[nodiscard]] const char* urlCStr() const noexcept { return _url.c_str(); }

  // The origin of the URL of the page ("https://example.com:8443"), the key of its storage.
  [[nodiscard]] std::string_view origin() const noexcept { return _url.origin(); }

  // The address space of the server of the document: its requests and navigations reach only the servers of this
  // address space or of a more public one (see PageRequestContext::addressSpace).
  [[nodiscard]] AddressSpace addressSpace() const noexcept { return _addressSpace; }

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

  // Keeps a reference on 'value' until the page dies: a new node wrapper (a node always has the same wrapper), the
  // globals of a lazy part of the prelude.
  void keep(JSValueConst value) { _kept.keep(value); }

  // The atom of Symbol.hasInstance, a well-known symbol: a predefined atom of QuickJS-ng, never freed. Read at the
  // setup of the page, before a script can replace the global Symbol.
  [[nodiscard]] JSAtom hasInstanceAtom() const noexcept { return _hasInstanceAtom; }

  // The object of the globals of the lazy part 'part' of the prelude, run on its first use in the page (see
  // LazyPrelude), kept until the page dies. JS_EXCEPTION, the exception pending, on memory or stack exhaustion.
  [[nodiscard]] JSValue lazyPart(LazyPrelude part);

  // The object of the lazy globals, in the prototype chain of the window (see InstallWindow()): the globals set up on
  // their first read are defined on it.
  [[nodiscard]] JSValueConst lazyGlobals() const noexcept { return _lazyGlobals.get(); }
  void setLazyGlobals(JsValue lazyGlobals) noexcept { _lazyGlobals = std::move(lazyGlobals); }

  // A script changed the tree or an attribute: the selector of run() is checked again after the task.
  void touchDom() noexcept { _selector.touch(); }

  // Whether pNode is in the document (DOM "connected").
  [[nodiscard]] bool connected(const lxb_dom_node* pNode) const noexcept;

  // pElement was created by a script (createElement(), createElementNS()): a script element runs once connected.
  void elementCreated(lxb_dom_node* pElement) { _scripts.elementCreated(pElement); }

  // Whether a script element created by a script has not started: cloneNode() reports its copies to nodeCloned() only
  // then.
  [[nodiscard]] bool hasCreatedScripts() const noexcept { return _scripts.hasCreated(); }

  // pCopy was cloned from pSource by a script (cloneNode(), each node of a deep copy).
  void nodeCloned(const lxb_dom_node* pSource, lxb_dom_node* pCopy) { _scripts.cloned(pSource, pCopy); }

  // A script inserted the nodes from pFirst to pEnd (excluded, null for after the last one), siblings: the tree
  // changed, and the scripts that scripts created among them and their descendants start once connected (HTML "prepare
  // the script element"), also their parent when it is such a script ("a node is inserted into the script element"),
  // after them. An inline script runs now, before this returns, an external one is fetched and run as a task. Returns
  // false when an uncatchable error (the deadline, memory exhaustion) stopped an inline one, or when the page cannot
  // hold the copy of one to run as a task (an out of memory error): the error is pending, for the native function that
  // inserted the nodes to fail with it, so that it stops the script that called it too.
  [[nodiscard]] bool nodesInserted(lxb_dom_node* pFirst, lxb_dom_node* pEnd) {
    touchDom();
    return !_scripts.hasCreated() || startInsertedScripts(pFirst, pEnd);
  }

  // A script changed the children, the text or the src of pElement: a script element created by a script, connected,
  // starts if it has not yet (it was empty when inserted). Returns false as nodesInserted().
  [[nodiscard]] bool scriptChanged(lxb_dom_node* pElement) {
    return !_scripts.hasCreated() || !_scripts.isCreated(pElement) || startScript(pElement);
  }

  // Whether the deadline stopped a script (see JsEngine::interrupted()): a native function that caught its error (the
  // conversion of a value) fails with ThrowInterrupted() instead, so that the scripts stop.
  [[nodiscard]] bool interrupted() const noexcept { return _engine.interrupted(); }

  // Whether the deadline has passed: no request starts any more.
  [[nodiscard]] bool expired() const noexcept { return _engine.expired(); }

  // Counts the 'size' bytes of the code of a script about to run in WebClientConfig::maxScriptBytes: false, nothing
  // counted, when they exceed it with those of the scripts run before.
  [[nodiscard]] bool admitScriptBytes(std::size_t size) noexcept {
    return _scripts.admit(size, session().config.maxScriptBytes);
  }

  // 'code' (null-terminated at its size) compiled as a classic script, or as a module for 'module' (its imports
  // resolved, but those of a module read back), named 'filename': read back from the script cache of the session when
  // it has it, else compiled (and cached when large enough). JS_EXCEPTION, the exception pending, when it does not
  // compile.
  [[nodiscard]] JsValue compileScript(std::string_view code, const char* filename, bool module) const;

  // The modules of the page (see js-module.cpp).
  [[nodiscard]] ModuleMap& modules() noexcept { return _modules; }

  // Forgets the rejection of 'promise', which the page handles: it is not reported as unhandled.
  void forgetRejection(JSValueConst promise) noexcept { _rejections.track(promise, JS_UNDEFINED, true); }

  // Reports the pending exception, unless it is that of the deadline: 'context' names its source for the log.
  void reportPendingException(const char* context);

  // Appends 'html', followed by a line feed for 'newLine' (writeln()), to the output of document.write() of the
  // running script, inserted after it when it ends. Ignored outside of a script run by the page loop. Returns false,
  // nothing appended, when the page cannot hold it.
  [[nodiscard]] bool documentWrite(std::string_view html, bool newLine);

  // The URL parsed from 'input' against the base URL, null-terminated, empty when it is not a valid URL. Valid until
  // the next resolution.
  [[nodiscard]] std::string_view resolveUrl(std::string_view input) { return _urlResolver.resolve(baseUrl(), input); }

  // The URL resolver of the page, whose views are valid until its next resolution.
  [[nodiscard]] UrlResolver& urlResolver() noexcept { return _urlResolver; }

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

  // The requests of the scripts not sent yet (fetch(), XMLHttpRequest).
  [[nodiscard]] RequestQueue& requests() noexcept { return _requests; }

  // The native memory that the scripts make the page hold.
  [[nodiscard]] MemoryBudget& memory() noexcept { return _memory; }

  // The buffer of the bindings that read the document through HtmlElement (textContent, innerHTML...), reused.
  [[nodiscard]] std::string& htmlBuffer() noexcept { return _htmlBuffer; }

  // The buffer of the bindings that decode text (TextDecoder, the bodies of the responses), reused.
  [[nodiscard]] RawChars& textBuffer() noexcept { return _textBuffer; }

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

  // The URL of the src attribute of 'script', resolved: empty when invalid, or empty itself (an error, not the URL of
  // the document: HTML "prepare the script element"). Valid until the next resolution.
  [[nodiscard]] std::string_view scriptUrl(const HtmlElement& script);

  // Runs 'script', a script of the parser: a classic one fetched first if external, followed by the output of its
  // document.write() calls, or a module one.
  void runParserScript(const ScriptQueue::ParserScript& script);

  // Runs 'script', a script inserted by a script to run as a task, within the WebClientConfig::maxRequests of the
  // document for an external one.
  void runInsertedScript(const ScriptQueue::InsertedScript& script);

  // Runs pScript, an external module script of 'url' (empty: an error) whose graph is fetched with 'credentials', then
  // dispatches its error event if its graph could not be fetched, else its load event.
  void runExternalModule(lxb_dom_node* pScript, std::string_view url, RequestCredentials credentials);

  // Runs pScript, an inline module script whose code is in _code, its imports fetched with 'credentials': its error
  // event if its graph could not be fetched.
  void runInlineModule(lxb_dom_node* pScript, RequestCredentials credentials);

  // Fetches pScript from 'url' (empty: an error) with its 'charset' attribute and 'credentials', and runs it: its error
  // event when it cannot be fetched, else its load event after it ran, and after the output of its document.write()
  // calls for a 'parserInserted' one.
  void runExternalScript(lxb_dom_node* pScript, std::string_view url, std::string_view charset,
                         RequestCredentials credentials, bool parserInserted);

  // Counts the code of _code, of the script 'name' (its URL, or "inline"), in WebClientConfig::maxScriptBytes. Returns
  // false, with the error reported, when it exceeds it: the script does not run.
  [[nodiscard]] bool admitCode(std::string_view name);

  // Runs _code, the code of pScript, as the running script named 'filename' (null-terminated): a 'parserInserted' one
  // is the writer of document.write(). Its errors are reported, but for an uncatchable error of a 'nested' script (an
  // inline script inserted by a script), left pending (see nodesInserted()): returns false then.
  [[nodiscard]] bool runCode(lxb_dom_node* pScript, const char* filename, bool parserInserted, bool nested);

  // See nodesInserted().
  [[nodiscard]] bool startInsertedScripts(lxb_dom_node* pFirst, lxb_dom_node* pEnd);

  // Starts pScript, created by a script, if connected and runnable (see ScriptQueue::start()). Returns false as
  // nodesInserted().
  [[nodiscard]] bool startScript(lxb_dom_node* pScript);

  // Queues 'script', inserted by a script, to run as a task, its URL and code counted in the native memory of the
  // page until it runs. Returns false, an out of memory error thrown, when the page cannot hold them.
  [[nodiscard]] bool queueInsertedScript(const ScriptQueue::InsertedScript& script);

  // Runs 'code' (null-terminated at its size) as a classic script named 'filename', from the bytecode of the script
  // cache of the session when it has it, else compiled (and cached when large enough). Returns its completion value,
  // JS_EXCEPTION with the exception pending.
  [[nodiscard]] JsValue evalScript(std::string_view code, const char* filename);

  // Makes 'code' (can be the content of _code) the null-terminated content of _code, the code that eval() runs.
  void setCode(std::string_view code);

  // Copies 'filename' (cannot view _code), null-terminated, after the null terminator of the code of _code, whose size
  // does not change: the file name of eval(), valid until the next setCode(). The buffer of the URL resolver would not
  // do: QuickJS reads the file name while it parses, and the script can resolve URLs when it runs. Returns the copy.
  [[nodiscard]] std::string_view setCodeFilename(std::string_view filename);

  // Fetches the script of 'url' whose charset attribute is 'charset' (can be empty) into _code, decoded to UTF-8, with
  // 'credentials' (those of its crossorigin attribute).
  bool fetchScript(std::string_view url, std::string_view charset, RequestCredentials credentials);

  void flushDocumentWrite(lxb_dom_node* pScript);

  // Dispatches a trusted event 'type' at pNode, at the window if null.
  void dispatchEvent(lxb_dom_node* pNode, const char* type, bool bubbles);

  // Runs the earliest timer. Returns false for a cleared timer: no task ran.
  bool runNextTimer();

  // Runs the microtasks, then reports the rejected promises left without handler.
  void runMicrotasks();

  TaskEnd endTask();

  // The tasks due at the current time of the virtual clock: the timers due, then the requests of the scripts, in turn
  // until none is left.
  TaskEnd runDueTasks();

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
  RawChars _textBuffer;     // see textBuffer()
  std::string _htmlBuffer;  // the buffer type of HtmlElement
  MemoryBudget _memory;
  SelectorWatch _selector;
  lxb_dom_node* _pDocumentNode;
  JSClassID _firstNodeClass{};
  JSAtom _hasInstanceAtom{};  // see hasInstanceAtom()
  http::StatusCode _status;
  StopReason _stopReason{StopReason::StateReached};
  ReadyState _readyState{ReadyState::Loading};
  AddressSpace _addressSpace;

  // Last: destroyed first, with every JavaScript value, before the document whose nodes the wrappers point to.
  JsEngine _engine;
  JsValues _kept{_engine.context()};  // see keep()
  JsValue _location;                  // window.location
  JsValue _dispatch;                  // the dispatch function of the prelude
  JsValue _natives;                   // the native helpers of the prelude, the first argument of its lazy parts
  JsValue _shared;                    // the helpers of the prelude for its lazy parts, their second argument
  JsValue _lazyGlobals;               // see lazyGlobals()
  std::array<JSValue, kNbLazyPreludes> _lazyParts = UndefinedValues<kNbLazyPreludes>();  // kept in _kept once run
  TimerQueue _timers;
  RequestQueue _requests{_engine.context(), _memory};
  RejectionTracker _rejections{_engine.context()};
  ModuleMap _modules{_engine.context()};
};

}  // namespace aeronet::internal
