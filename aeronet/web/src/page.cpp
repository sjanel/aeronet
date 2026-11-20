// The page loop: the scripts of a document in the order of a browser, its events, microtasks and timers, between
// which the stop conditions of navigate() are checked.

#include "page.hpp"

#include <lexbor/dom/interfaces/node.h>
#include <lexbor/tag/const.h>
#include <quickjs.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/cctype.hpp"
#include "aeronet/client-connection.hpp"
#include "aeronet/css-selector.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/html-element.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/web-session.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/web-client.hpp"
#include "aeronet/web-page.hpp"
#include "html-node-access.hpp"
#include "html-node-ops.hpp"
#include "html-tree-walk.hpp"
#include "js-dom.hpp"
#include "js-engine.hpp"
#include "js-prelude.hpp"
#include "js-window.hpp"
#include "memory-budget.hpp"
#include "rejection-tracker.hpp"
#include "script-queue.hpp"
#include "timer-queue.hpp"
#include "uncaught-errors.hpp"
#include "web-url.hpp"

namespace aeronet::internal {

namespace {

std::string_view SkipWhitespace(std::string_view str) noexcept {
  return str.substr(
      std::min(str.size(), static_cast<std::size_t>(std::ranges::find_if_not(str, IsAsciiWhitespace) - str.begin())));
}

// Parses the content of a <meta http-equiv="refresh"> (HTML "shared declarative refresh steps"): "5",
// "0; url=/next", "3, URL='/x'". Returns false when it is not valid.
bool ParseRefresh(std::string_view content, int64_t& seconds, std::string_view& url) noexcept {
  content = SkipWhitespace(content);
  std::size_t pos = 0;
  seconds = 0;
  for (; pos < content.size() && content[pos] >= '0' && content[pos] <= '9'; ++pos) {
    seconds = std::min<int64_t>((seconds * 10) + (content[pos] - '0'), std::numeric_limits<int32_t>::max());
  }
  if (pos == 0 && !content.starts_with('.')) {
    return false;
  }
  while (pos < content.size() && ((content[pos] >= '0' && content[pos] <= '9') || content[pos] == '.')) {
    ++pos;  // the fraction is ignored
  }
  content = SkipWhitespace(content.substr(pos));
  if (content.starts_with(';') || content.starts_with(',')) {
    content = SkipWhitespace(content.substr(1));
  }
  if (content.size() >= 3 && CaseInsensitiveEqual(content.substr(0, 3), "url")) {
    const std::string_view afterUrl = SkipWhitespace(content.substr(3));
    if (afterUrl.starts_with('=')) {
      content = SkipWhitespace(afterUrl.substr(1));
    }
  }
  if (content.starts_with('"') || content.starts_with('\'')) {
    const char quote = content.front();
    content.remove_prefix(1);
    content = content.substr(0, content.find(quote));
  }
  url = content;
  return true;
}

// Throws in pCtx the SyntaxError of 'input', a URL that cannot be parsed.
void ThrowInvalidUrl(JSContext* pCtx, std::string_view input) noexcept {
  JS_ThrowDOMException(pCtx, "SyntaxError", "Invalid URL '%.*s'", static_cast<int>(input.size()), input.data());
}

struct JsFreeDeleter {
  void operator()(uint8_t* pData) const noexcept { js_free(pCtx, pData); }

  JSContext* pCtx;
};

// The function of the prelude: compiled by the first page of a session, which keeps its bytecode for the next ones.
// The compilation ignores the stack limit 'maxStackSize' of the page: the parser of QuickJS-ng reports a stack overflow
// in its lookahead as a bogus syntax error (js_parse_skip_parens_token drops the failure of next_token()), and the
// prelude, code of aeronet, has a bounded depth. Returns the function, owned, or JS_EXCEPTION with the exception
// pending.
JSValue PreludeFunction(JSContext* pCtx, RawBytes& bytecode, std::size_t maxStackSize) {
  if (bytecode.empty()) {
    const std::string_view source = PreludeSource();
    JSRuntime* pRuntime = JS_GetRuntime(pCtx);
    JS_SetMaxStackSize(pRuntime, 0);  // no limit
    JsValue compiled(pCtx, JS_Eval(pCtx, source.data(), source.size(), "aeronet:prelude",
                                   JS_EVAL_FLAG_COMPILE_ONLY));  // of type JS_EVAL_TYPE_GLOBAL, 0
    JS_SetMaxStackSize(pRuntime, maxStackSize);
    if (JS_IsException(compiled.get())) {
      return JS_EXCEPTION;
    }
    std::size_t size = 0;
    const std::unique_ptr<uint8_t, JsFreeDeleter> pData(
        JS_WriteObject(pCtx, &size, compiled.get(), JS_WRITE_OBJ_BYTECODE), JsFreeDeleter{pCtx});
    if (!pData) {
      return JS_EXCEPTION;
    }
    bytecode = RawBytes(reinterpret_cast<const std::byte*>(pData.get()), size);
    return JS_EvalFunction(pCtx, compiled.release());
  }
  const JSValue compiled =
      JS_ReadObject(pCtx, reinterpret_cast<const uint8_t*>(bytecode.data()), bytecode.size(), JS_READ_OBJ_BYTECODE);
  return JS_IsException(compiled) ? JS_EXCEPTION : JS_EvalFunction(pCtx, compiled);
}

}  // namespace

SteadyClock::time_point DeadlineIn(std::chrono::milliseconds timeout) noexcept {
  const SteadyClock::time_point now = SteadyClock::now();
  if (timeout <= std::chrono::milliseconds::zero()) {
    return now;
  }
  if (timeout >= std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::time_point::max() - now)) {
    return SteadyClock::time_point::max();
  }
  return now + timeout;
}

bool SelectorWatch::changedAndMatches(const HtmlDocument& document) {
  if (_pSelector == nullptr || _version == _checkedVersion) {
    return false;
  }
  _checkedVersion = _version;
  return document.querySelector(*_pSelector).valid();
}

Page::Page(WebSession& session, std::string_view url, http::StatusCode status, std::string_view html,
           std::string_view contentType)
    : _session(session),
      _document(HtmlNodeAccess::ParseWithScripting(html, contentType)),
      _url(url),
      _memory(session.config.maxScriptMemory),
      _pDocumentNode(HtmlNodeAccess::Node(_document)),
      _status(status),
      _engine(JsLimits{session.config.maxScriptMemory, session.config.maxScriptStackSize}),
      _timers(_engine.context(), _memory, session.config.maxTimerDelay.count(), session.config.maxIntervalRuns) {
  if (const HtmlElement base = _document.querySelector("base[href]")) {
    _baseUrl.assign(resolveUrl(base.attribute("href")));
  }
  JS_SetContextOpaque(context(), this);
  JS_SetHostPromiseRejectionTracker(_engine.runtime(), &TrackRejection, this);
  installBindings();
}

Page::~Page() = default;

void Page::installBindings() {
  // The setup runs the code of aeronet only: no deadline.
  _engine.enter(SteadyClock::time_point::max());
  if (!tryInstallBindings()) {
    // Only on memory or stack exhaustion: the limit is too low for the page setup.
    throw std::runtime_error("Cannot set up the JavaScript runtime of a page: " + _engine.takeException());
  }
}

bool Page::tryInstallBindings() {
  JSContext* pCtx = context();
  if (!InstallDom(*this)) {
    return false;
  }
  const JsValue natives(pCtx, InstallWindow(*this));
  if (JS_IsException(natives.get())) {
    return false;
  }
  const JsValue prelude(pCtx, PreludeFunction(pCtx, session().preludeBytecode, session().config.maxScriptStackSize));
  if (JS_IsException(prelude.get())) {
    return false;
  }
  JSValueConst argument = natives.get();
  const JsValue internals(pCtx, JS_Call(pCtx, prelude.get(), JS_UNDEFINED, 1, &argument));
  if (JS_IsException(internals.get())) {
    return false;
  }
  _dispatch = JsValue(pCtx, JS_GetPropertyStr(pCtx, internals.get(), "dispatch"));
  return !JS_IsException(_dispatch.get());
}

void Page::reportError(std::string_view text) {
  log::debug("[js {}] uncaught error: {}", url(), text.substr(0, UncaughtErrors::kMaxSize));
  _errors.add(text);
}

void Page::reportPendingException(const char* context) {
  const std::string error = _engine.takeException();
  // The deadline stops the scripts with an uncatchable error: the page reports it as its stop reason.
  if (!_engine.interrupted()) {
    reportError(error);
  } else {
    log::debug("[js {}] {} stopped by the deadline", url(), context);
  }
}

bool Page::documentWrite(std::string_view html, bool newLine) {
  if (_scripts.current() == nullptr) {
    log::debug("[js {}] document.write() outside of a script of the parser ignored", url());
    return true;
  }
  const std::size_t size = html.size() + static_cast<std::size_t>(newLine);
  // Counted as the nodes it can become, until it is parsed.
  const std::size_t memory = size * kHtmlMemoryPerByte;
  if (!_memory.canHold(memory)) {
    return false;
  }
  RawChars& written = _scripts.written();
  written.ensureAvailableCapacityExponential(size);
  written.unchecked_append(html);
  if (newLine) {
    written.unchecked_push_back('\n');
  }
  _memory.charge(memory);
  return true;
}

bool Page::navigateTo(std::initializer_list<std::string_view> inputParts) {
  const std::string_view url = _urlResolver.resolve(baseUrl(), inputParts);
  return startNavigation(url, _urlResolver.concatenatedInput());
}

bool Page::startNavigation(std::string_view url, std::string_view input) {
  if (url.empty()) {
    ThrowInvalidUrl(context(), input);
    return false;
  }

  if (!IsHttpUrl(url)) {
    log::debug("[js {}] navigation to {} ignored: not an http or https URL", this->url(), url);
  } else if (url.contains('#') && WithoutFragment(url) == WithoutFragment(this->url())) {
    // A navigation to a fragment of the document only changes the URL (HTML "navigate to a fragment").
    _url.assign(url);
  } else {
    _pendingNavigation.assign(url);
  }

  return true;
}

bool Page::replaceUrl(std::string_view input) {
  const std::string_view url = resolveUrl(input);
  if (url.empty()) {
    ThrowInvalidUrl(context(), input);
    return false;
  }

  if (!_url.isSameOrigin(url)) {
    JS_ThrowDOMException(context(), "SecurityError", "The URL '%.*s' is not of the origin of the document",
                         static_cast<int>(url.size()), url.data());
    return false;
  }

  _url.assign(url);
  return true;
}

bool Page::runNextTimer() {
  JSContext* pCtx = context();
  TimerTask task(pCtx);

  if (!_timers.takeNext(task)) {
    return false;
  }

  JsValue result;
  if (JS_IsString(task.callback)) {
    const JsString code(pCtx, task.callback);
    result = code.failed() ? JsValue(pCtx, JS_EXCEPTION) : _engine.eval(code.view(), _url.c_str());
  } else {
    const JsValue global(pCtx, JS_GetGlobalObject(pCtx));
    result =
        JsValue(pCtx, JS_Call(pCtx, task.callback, global.get(), static_cast<int>(task.args.size()), task.args.data()));
  }

  if (JS_IsException(result.get())) {
    reportPendingException("timer");
  }

  // The nesting level stays until the microtasks of the task have run (see endTask()): a timer set by them is nested.
  return true;
}

void Page::dispatchEvent(lxb_dom_node_t* pNode, const char* type, bool bubbles) {
  JSContext* pCtx = context();

  const JsValue target(pCtx, pNode == nullptr ? JS_GetGlobalObject(pCtx) : wrap(pNode));
  const JsValue typeValue(pCtx, JS_NewString(pCtx, type));

  if (!JS_IsException(target.get()) && !JS_IsException(typeValue.get())) {
    JSValueConst args[] = {target.get(), typeValue.get(), JS_NewBool(pCtx, bubbles), JS_FALSE};
    const JsValue result(pCtx, JS_Call(pCtx, _dispatch.get(), JS_UNDEFINED, 4, args));
    if (!JS_IsException(result.get())) {
      return;
    }
  }

  reportPendingException(type);
}

void Page::TrackRejection([[maybe_unused]] JSContext* pCtx, JSValueConst promise, JSValueConst reason, bool isHandled,
                          void* pPage) noexcept {
  static_cast<Page*>(pPage)->_rejections.track(promise, reason, isHandled);
}

void Page::runMicrotasks() {
  _engine.runMicrotasks([this](std::string_view error) { reportError(error); });
  _rejections.flush([](void* pPage, std::string_view text) { static_cast<Page*>(pPage)->reportError(text); }, this);
}

Page::TaskEnd Page::endTask() {
  runMicrotasks();
  _timers.resetNesting();
  if (_engine.expired()) {
    _stopReason = StopReason::TimedOut;
    return TaskEnd::Stop;
  }
  if (!_pendingNavigation.empty()) {
    return TaskEnd::Navigate;
  }
  if (_selector.changedAndMatches(_document)) {
    _stopReason = StopReason::SelectorMatched;
    return TaskEnd::Stop;
  }
  return TaskEnd::Continue;
}

// The timers due at the current time of the virtual clock, as tasks. It does not advance while the document loads.
Page::TaskEnd Page::runDueTimers() {
  while (!_timers.empty() && _timers.nextDue() <= _timers.clock()) {
    if (runNextTimer()) {
      if (const TaskEnd end = endTask(); end != TaskEnd::Continue) {
        return end;
      }
    }
  }
  return TaskEnd::Continue;
}

void Page::setCode(std::string_view code) {
  if (code.data() != _code.data()) {
    _code.clear();
    _code.ensureAvailableCapacity(code.size() + 1U);
    _code.unchecked_append(code);
  }
  assert(code.size() == _code.size());
  _code.push_back('\0');
  _code.setSize(_code.size() - 1U);
}

std::string_view Page::setCodeFilename(std::string_view filename) {
  const auto codeSize = _code.size();
  _code.ensureAvailableCapacity(1U + filename.size() + 1U);
  _code.unchecked_push_back('\0');
  _code.unchecked_append(filename);
  _code.unchecked_push_back('\0');
  _code.setSize(codeSize);
  return {_code.data() + codeSize + 1U, filename.size()};
}

bool Page::fetchScript(std::string_view url, std::string_view charset) {
  // A valid URL that the HTTP client cannot request: not http or https, with user info, port 0, a too long target...
  if (!HttpRequest::IsValidUrl(url)) {
    log::debug("[js {}] script {} not fetched: the HTTP client does not support its URL", this->url(), url);
    return false;
  }
  const HttpClientResult result = session().http.get(url);
  if (!result) {
    log::debug("[js {}] script {} not fetched: {}", this->url(), url, ErrcToStr(result.error()));
    return false;
  }
  if (result->status() < 200 || result->status() >= 300) {
    log::debug("[js {}] script {} not run: status {}", this->url(), url, result->status());
    return false;
  }
  // Decoded as browsers decode classic scripts: QuickJS-ng only reads UTF-8.
  _code.clear();
  setCode(DecodeScript(result->bodyInMemory(), result->headerValueOrEmpty(http::ContentType), charset,
                       _document.encoding(), _code));
  return true;
}

void Page::runScript(lxb_dom_node_t* pScript) {
  const HtmlElement script = HtmlNodeAccess::Element(pScript);
  const bool external = script.hasAttribute("src");

  // The URL of an external script, null-terminated: in the buffer of the URL resolver, then after the code in _code.
  std::string_view scriptUrl;
  if (external) {
    // An empty src is an error, not the URL of the document (HTML "prepare the script element").
    const std::string_view src = script.attribute("src");
    if (!src.empty()) {
      scriptUrl = resolveUrl(src);
    }
    if (scriptUrl.empty() || !fetchScript(scriptUrl, script.attribute("charset"))) {
      dispatchEvent(pScript, "error", false);
      return;
    }
    scriptUrl = setCodeFilename(scriptUrl);
  } else {
    setCode(script.textContent(_htmlBuffer));
  }
  const std::string_view code(_code);
  if (!_scripts.admit(code.size(), session().config.maxScriptBytes)) {
    reportError(std::string("aeronet: script ")
                    .append(external ? scriptUrl : std::string_view("inline"))
                    .append(" not run: the scripts of the document exceed WebClientConfig::maxScriptBytes"));
    return;
  }

  _scripts.setCurrent(pScript);
  const JsValue result = _engine.eval(code, external ? scriptUrl.data() : _url.c_str());
  if (JS_IsException(result.get())) {
    reportPendingException("script");
  }
  _scripts.setCurrent(nullptr);
  flushDocumentWrite(pScript);
  if (external) {
    dispatchEvent(pScript, "load", false);
  }
}

void Page::flushDocumentWrite(lxb_dom_node_t* pScript) {
  RawChars& written = _scripts.written();
  if (written.empty()) {
    return;
  }

  _memory.release(written.size() * kHtmlMemoryPerByte);

  // Parsed as a fragment in the context of the parent of the script, inserted after it, before point.pReference.
  const InsertionPoint point = AdjacentInsertionPoint(pScript, HtmlInsertPosition::AfterEnd);
  if (!CanInsertAdjacent(point)) {
    written.clear();
    log::debug("[js {}] document.write() output dropped: its script is not in an element", url());
    return;
  }

  // A failed allocation ends the navigation, and the page with its output: it is only cleared on the other paths.
  HtmlNodeAccess::Element(pScript).insertAdjacentHtml(HtmlInsertPosition::AfterEnd, written);

  written.clear();

  _memory.charge(NodesMemory(pScript->next, point.pReference));
  if (_memory.exceeded()) {
    reportError("aeronet: the output of document.write() exceeds WebClientConfig::maxScriptMemory");
  }
  _scripts.collect(pScript->next, point.pReference, url());
  touchDom();
}

Page::Refresh Page::findRefresh() {
  Refresh refresh{{}, -1};
  WalkDescendants(_pDocumentNode, [&](lxb_dom_node_t* pNode) {
    if (pNode->type != LXB_DOM_NODE_TYPE_ELEMENT || pNode->local_name != LXB_TAG_META) {
      return true;
    }
    const HtmlElement meta = HtmlNodeAccess::Element(pNode);
    int64_t seconds = 0;
    std::string_view target;
    if (!CaseInsensitiveEqual(meta.attribute("http-equiv"), "refresh") ||
        !ParseRefresh(meta.attribute("content"), seconds, target)) {
      return true;
    }
    refresh.url.assign(resolveUrl(target));
    refresh.delay = seconds * 1000;
    return false;  // the first one only
  });
  // A refresh of the page itself (auto-refreshing pages) is not followed: the page would load again and again.
  if (refresh.delay >= 0 && (!IsHttpUrl(refresh.url) || std::string_view(refresh.url) == url())) {
    refresh.delay = -1;
  }
  return refresh;
}

Page::Outcome Page::run(const CssSelector* pSelector, LoadState until, SteadyClock::time_point deadline) {
  _engine.enter(deadline);
  _selector.watch(pSelector);
  // Data served in the HTML costs no script run.
  if (pSelector != nullptr && _document.querySelector(*pSelector)) {
    _stopReason = StopReason::SelectorMatched;
    return outcome(TaskEnd::Stop);
  }
  _scripts.collect(_pDocumentNode->first_child, nullptr, url());

  TaskEnd end = TaskEnd::Continue;

  // Each step is a task, followed by the timers due at once.
  const auto step = [this, &end](auto task) {
    task();
    end = endTask();
    if (end == TaskEnd::Continue) {
      end = runDueTimers();
    }
    return end == TaskEnd::Continue;
  };

  const auto runScripts = [&](ScriptQueue::Kind kind) {
    for (lxb_dom_node_t* pScript = _scripts.next(kind); pScript != nullptr; pScript = _scripts.next(kind)) {
      if (!step([this, pScript] { runScript(pScript); })) {
        return false;
      }
    }
    return true;
  };

  const bool domContentLoaded = runScripts(ScriptQueue::Kind::Blocking) && step([this] {
                                  _readyState = ReadyState::Interactive;
                                  dispatchEvent(_pDocumentNode, "readystatechange", false);
                                }) &&
                                runScripts(ScriptQueue::Kind::Defer) &&
                                step([this] { dispatchEvent(_pDocumentNode, "DOMContentLoaded", true); });
  if (!domContentLoaded) {
    return outcome(end);
  }
  if (until == LoadState::DomContentLoaded) {
    return outcome(TaskEnd::Stop);
  }

  const bool loaded = runScripts(ScriptQueue::Kind::Async) && step([this] {
                        _readyState = ReadyState::Complete;
                        dispatchEvent(_pDocumentNode, "readystatechange", false);
                      }) &&
                      step([this] { dispatchEvent(nullptr, "load", false); });
  if (!loaded) {
    return outcome(end);
  }

  Refresh refresh = findRefresh();
  if (refresh.delay == 0) {
    _pendingNavigation = std::move(refresh.url);
    return outcome(TaskEnd::Navigate);
  }
  if (until == LoadState::Load) {
    return outcome(TaskEnd::Stop);
  }
  // The virtual clock jumps to each timer in turn, until the refresh of the document if any.
  while (!_timers.empty() && (refresh.delay < 0 || _timers.nextDue() < refresh.delay)) {
    if (!runNextTimer()) {
      continue;
    }
    end = endTask();
    if (end != TaskEnd::Continue) {
      return outcome(end);
    }
  }
  if (refresh.delay > 0 && refresh.delay <= session().config.maxTimerDelay.count()) {
    _pendingNavigation = std::move(refresh.url);
    return outcome(TaskEnd::Navigate);
  }
  return outcome(TaskEnd::Stop);
}

std::expected<std::string, std::string> Page::evaluate(std::string_view script, std::chrono::milliseconds timeout) {
  _engine.enter(DeadlineIn(timeout));

  JSContext* pCtx = context();

  setCode(script);

  std::expected<std::string, std::string> ret;
  // The JSON of the completion value of the script into 'ret'. Returns false, the exception pending, if it throws.
  const auto run = [this, pCtx, &ret] {
    const JsValue result(_engine.eval(std::string_view(_code), "aeronet:evaluate"));
    if (JS_IsException(result.get())) {
      return false;
    }
    const JsValue json(pCtx, JS_JSONStringify(pCtx, result.get(), JS_UNDEFINED, JS_UNDEFINED));
    if (JS_IsException(json.get())) {
      return false;
    }
    if (!JS_IsUndefined(json.get())) {
      const JsString text(pCtx, json.get());
      if (text.failed()) {
        return false;
      }
      ret.value() = text.view();
    }
    return true;
  };

  if (!run()) {
    std::string error = _engine.takeException();
    ret = std::unexpected(_engine.interrupted()
                              ? "The script ran longer than its timeout of " + std::to_string(timeout.count()) + " ms"
                              : std::move(error));
  }

  runMicrotasks();

  if (!_pendingNavigation.empty()) {
    log::debug("[js {}] navigation to {} started by evaluate() ignored", url(), std::string_view(_pendingNavigation));
    _pendingNavigation.clear();
  }
  return ret;
}

}  // namespace aeronet::internal
