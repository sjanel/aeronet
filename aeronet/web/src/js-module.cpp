// The ES modules of a page: its module scripts and the import() of its scripts, whose graphs load synchronously over
// its HTTP client, through the hooks that QuickJS-ng calls when it resolves the imports of a module (see ModuleMap for
// why they never fail inside a resolution).

#include "js-module.hpp"

#include <quickjs.h>

#include <cassert>
#include <cstdint>
#include <span>
#include <string_view>

#include "aeronet/http-client.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/log.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/web-client-config.hpp"
#include "cors.hpp"
#include "html-encoding.hpp"
#include "html-node-ops.hpp"
#include "js-engine.hpp"
#include "js-network.hpp"
#include "module-map.hpp"
#include "page.hpp"
#include "request-queue.hpp"
#include "script-queue.hpp"
#include "web-url.hpp"

namespace aeronet::internal {

namespace {

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

// HTML "resolve a module specifier", without import map: an absolute URL, or a URL relative to 'base' starting with
// "/", "./" or "../". Empty for a bare specifier ("react") or an invalid URL. Valid until the next resolution.
std::string_view ResolveSpecifier(UrlResolver& resolver, std::string_view base, std::string_view specifier) {
  if (specifier.starts_with('/') || specifier.starts_with("./") || specifier.starts_with("../")) {
    return resolver.resolve(base, specifier);
  }
  return resolver.resolve({}, specifier);
}

// Whether the script or module named 'name' (its file name) was fetched: not an inline script of the document (named
// after it), nor code without URL (evaluate(), Function()).
bool IsExternal(Page& page, std::string_view name) noexcept { return name != page.url() && IsHttpUrl(name); }

// The base URL of the imports of the script or module named 'name': the URL of the response of a redirected module,
// 'name' for another external one, else the base URL of the document.
std::string_view BaseUrlOf(Page& page, std::string_view name) noexcept {
  if (const std::string_view url = page.modules().redirectedUrl(name); !url.empty()) {
    return url;
  }
  return IsExternal(page, name) ? name : page.baseUrl();
}

// The TypeError of a module specifier that does not resolve, thrown in pCtx. Returns JS_EXCEPTION.
JSValue ThrowUnresolved(JSContext* pCtx, std::string_view specifier) noexcept {
  return JS_ThrowTypeError(pCtx,
                           "Failed to resolve module specifier '%.*s': a relative reference must start with '/', './' "
                           "or '../' (import maps are not supported)",
                           static_cast<int>(specifier.size()), specifier.data());
}

// The exception pending in pCtx, taken: the error of a failure of a graph.
JSValue TakeError(JSContext* pCtx) noexcept { return JS_GetException(pCtx); }

// The TypeError of the module 'url' that could not be fetched for 'reason', as a value.
JSValue FetchError(Page& page, std::string_view url, std::string_view reason) {
  log::debug("[js {}] module {} not loaded: {}", page.url(), url, reason);
  JS_ThrowTypeError(page.context(), "Failed to fetch module '%.*s': %.*s", static_cast<int>(url.size()), url.data(),
                    static_cast<int>(reason.size()), reason.data());
  return TakeError(page.context());
}

// The TypeError of a page whose module map is broken, as a value.
JSValue BrokenError(JSContext* pCtx) noexcept {
  JS_ThrowTypeError(pCtx, "aeronet: no module loads in this page any more, after a memory exhaustion");
  return TakeError(pCtx);
}

// The TypeError of the module 'url' whose graph an uncatchable error stopped (see ModuleMap::FailedModule::stopped()),
// as a value.
JSValue StoppedError(JSContext* pCtx, std::string_view url) noexcept {
  JS_ThrowTypeError(pCtx,
                    "aeronet: module '%.*s' not loaded: the deadline or a memory exhaustion stopped the load of its "
                    "graph",
                    static_cast<int>(url.size()), url.data());
  return TakeError(pCtx);
}

// The evaluation of kFailedModuleName: never reached, as a graph with a failure is not evaluated, nor are its modules
// later (see ModuleMap::failed()), but when a memory exhaustion of the C++ code ends a graph before they are recorded.
int ThrowFailedModule(JSContext* pCtx, JSModuleDef* /*pModule*/) noexcept {
  JS_ThrowTypeError(pCtx, "aeronet: a module of this graph failed to load");
  return -1;
}

// import.meta.resolve(specifier) of a module whose base URL is pData[0]: the URL of the module of 'specifier'.
JSValue ImportMetaResolve(JSContext* pCtx, JSValueConst /*self*/, int argc, JSValueConst* argv, int /*magic*/,
                          JSValueConst* pData) noexcept {
  try {
    // Converted first: the conversion can run a script, which can resolve URLs.
    const JsArgument specifier(pCtx, argc, argv, 0);
    if (specifier.failed()) {
      return JS_EXCEPTION;
    }

    const JsString base(pCtx, pData[0]);
    if (base.failed()) {
      return JS_EXCEPTION;
    }
    const std::string_view url = ResolveSpecifier(Page::Of(pCtx).urlResolver(), base.view(), specifier.view());
    return url.empty() ? ThrowUnresolved(pCtx, specifier.view()) : NewString(pCtx, url);
  } catch (...) {
    return JS_ThrowOutOfMemory(pCtx);
  }
}

// Sets the properties of the import.meta of pModule (HTML "HostGetImportMetaProperties"): its URL 'url', and
// resolve(). Returns false on memory exhaustion, the exception pending.
bool SetImportMeta(JSContext* pCtx, JSModuleDef* pModule, std::string_view url) noexcept {
  const JsValue meta(pCtx, JS_GetImportMeta(pCtx, pModule));
  if (JS_IsException(meta.get())) {
    return false;
  }
  const JsValue urlValue(pCtx, NewString(pCtx, url));
  if (JS_IsException(urlValue.get())) {
    return false;
  }
  JSValueConst data = urlValue.get();
  return Define(pCtx, meta.get(), "url", JS_DupValue(pCtx, urlValue.get()), kOrdinaryProperty) &&
         Define(pCtx, meta.get(), "resolve", JS_NewCFunctionData(pCtx, &ImportMetaResolve, 1, 0, 1, &data),
                kOrdinaryProperty);
}

// The module of 'code' (null-terminated at its size) named 'pName', compiled or read from the script cache, with its
// imports resolved: the hooks run nested, within its ModuleMap::Compilation. JS_EXCEPTION, the exception pending, when
// it does not compile, or when its resolution fails (memory exhaustion).
JsValue CompileModule(Page& page, const char* pName, std::string_view code) {
  JsValue module = page.compileScript(code, pName, true);

  // A compiled module is resolved already, one read from the cache is not.
  if (!JS_IsException(module.get()) && JS_ResolveModule(page.context(), module.get()) < 0) {
    return {page.context(), JS_EXCEPTION};
  }
  return module;
}

// A module of a graph, or the error of its failure.
struct Loaded {
  JSModuleDef* pModule;  // null on failure
  JSValue error;         // owned, of a failure
  bool fetchFailure;     // see ModuleMap::FailedModule
  bool remember;         // whether the next imports of the module fail with the same error without request
};

Loaded Failure(JSValue error, bool fetchFailure) noexcept { return {nullptr, error, fetchFailure, true}; }

// Fetches the module named 'name' (its URL) with the credentials of the graph, then compiles it and loads its imports.
Loaded FetchAndCompile(Page& page, const char* pName) {
  JSContext* pCtx = page.context();
  ModuleMap& modules = page.modules();
  const std::string_view name(pName);

  // NormalizeModuleName() fails every import of a broken map.
  assert(!modules.broken());
  if (page.expired()) {
    // Not remembered: a later evaluate() runs with another deadline.
    ThrowInterrupted(pCtx);
    return {nullptr, TakeError(pCtx), false, false};
  }

  // The root of a module script is counted by the page.
  const WebClientConfig& config = page.session().config;
  if ((!modules.atRoot() || !modules.topLevel()) && !page.requests().admit(config.maxRequests)) {
    return Failure(FetchError(page, name, "the scripts of the document exceed WebClientConfig::maxRequests"), true);
  }

  const std::string_view url = WithoutFragment(name);
  if (!HttpRequest::IsValidUrl(url)) {
    return Failure(FetchError(page, name, "the HTTP client does not support its URL"), true);
  }

  RequestQueue::Request request =
      ScriptRequest(page, url, RequestMode::Cors, modules.credentials(), modules.referrer());
  const FetchResult response = Fetch(page, request);
  if (!response) {
    return Failure(FetchError(page, name, response.error()), true);
  }

  if (response->status() < 200 || response->status() >= 300) {
    return Failure(FetchError(page, name, "its response is not a success"), true);
  }

  const std::string_view contentType = response->headerValueOrEmpty(http::ContentType);
  if (!IsJavaScriptMimeType(TrimOws(contentType.substr(0, contentType.find(';'))))) {
    return Failure(FetchError(page, name, "its Content-Type is not a JavaScript MIME type"), true);
  }

  // Its base URL, with the fragment of its name, which the requests drop.
  const std::string_view responseUrl = RequestUrl(page.urlResolver(), request.http, name.substr(url.size()));
  if (responseUrl != name) {
    modules.addRedirected(name, responseUrl);
  }

  // The code of a module is UTF-8 (HTML "fetch a single module script"), whatever its Content-Type says.
  std::string_view body = response->bodyInMemory();
  if (body.starts_with(kUtf8Bom)) {
    body.remove_prefix(kUtf8Bom.size());
  }

  const std::string_view code = modules.setCode(DecodeUtf8(body, page.textBuffer()));
  if (!page.admitScriptBytes(code.size())) {
    JS_ThrowRangeError(pCtx,
                       "aeronet: module '%s' not run: the scripts of the document exceed "
                       "WebClientConfig::maxScriptBytes",
                       pName);
    return Failure(TakeError(pCtx), false);
  }

  const uint32_t hookCalls = modules.hookCalls();
  const uint32_t failures = modules.failures();
  ModuleMap::Compilation compilation(modules, name);
  const JsValue module = CompileModule(page, pName, code);
  if (JS_IsException(module.get())) {
    // A resolution started: it cannot fail but on memory exhaustion, which may leave freed modules in the graph.
    if (modules.hookCalls() != hookCalls) {
      modules.setBroken();
    }
    // Not loaded: a later import compiles it again after the deadline or a memory exhaustion, not after an error of
    // its code.
    const JSValue error = TakeError(pCtx);
    return {nullptr, error, false, !JS_IsUncatchableError(error)};
  }

  if (modules.failures() != failures) {
    // An import failed: the module stays loaded, never evaluated, as its later imports fail too.
    return Failure(JS_DupValue(pCtx, modules.graphError()), modules.graphFetchFailed());
  }

  auto* pModule = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(module.get()));
  const std::string_view baseUrl = modules.redirectedUrl(name);
  if (!SetImportMeta(pCtx, pModule, baseUrl.empty() ? name : baseUrl)) {
    return Failure(TakeError(pCtx), false);
  }

  compilation.loaded();

  // QuickJS-ng keeps its module in its loaded modules: the value of the compilation is freed.
  return {pModule, JS_UNDEFINED, false, false};
}

// The hooks run under QuickJS-ng, which takes no C++ exception: they are memory exhaustion of the C++ code (see
// JsMethod), an out of memory error.

// Normalizes the import 'pSpecifier' of the module or script named 'pBaseName' into the name of its module: its URL,
// resolved against the base URL of 'pBaseName'. A specifier that does not resolve, a URL that cannot be fetched or a
// module that failed fails: at the root of a graph, with its error thrown (null returned), else recorded in the graph,
// the import resolved to kFailedModuleName.
char* NormalizeModuleName(JSContext* pCtx, const char* pBaseName, const char* pSpecifier, void* pPage) noexcept {
  Page& page = *static_cast<Page*>(pPage);
  ModuleMap& modules = page.modules();
  modules.countHookCall();
  try {
    if (modules.atRoot() && !modules.topLevel()) {
      // An import(): its graph is fetched with the credentials of a module script without crossorigin attribute.
      modules.beginGraph(RequestCredentials::SameOrigin);
    }
    const std::string_view baseName(pBaseName);
    const std::string_view specifier(pSpecifier);
    std::string_view name;
    JsValue error;
    bool fetchFailure = true;
    if (modules.broken()) {
      error = JsValue(pCtx, BrokenError(pCtx));
    } else {
      const std::string_view baseUrl = BaseUrlOf(page, baseName);
      name = ResolveSpecifier(page.urlResolver(), baseUrl, specifier);
      if (name.empty()) {
        ThrowUnresolved(pCtx, specifier);
        error = JsValue(pCtx, TakeError(pCtx));
        fetchFailure = false;
      } else if (!IsHttpUrl(name)) {
        error = JsValue(pCtx, FetchError(page, name, "only http and https URLs can be fetched"));
      } else if (const ModuleMap::FailedModule* pFailed = modules.failed(name)) {
        error = JsValue(pCtx, pFailed->stopped() ? StoppedError(pCtx, name) : JS_DupValue(pCtx, pFailed->error));
        fetchFailure = pFailed->fetchFailure;
      } else {
        modules.imported(name);
        // HTML "HostLoadImportedModule": the referrer is the base URL of the importing script, the document for the
        // root of a module script (named after its own URL) and for the scripts that it did not fetch.
        const bool fromDocument = (modules.atRoot() && modules.topLevel()) || !IsExternal(page, baseName);
        modules.setReferrer(fromDocument ? page.url() : baseUrl);
      }
    }
    if (error.context() != nullptr) {
      modules.fail(JS_DupValue(pCtx, error.get()), fetchFailure);
      if (modules.atRoot()) {
        JS_Throw(pCtx, error.release());
        return nullptr;
      }
      name = ModuleMap::kFailedModuleName;
    }
    // Null on memory exhaustion, the exception pending.
    return js_strndup(pCtx, name.data(), name.size());
  } catch (...) {
    JS_ThrowOutOfMemory(pCtx);
    return nullptr;
  }
}

// Loads the module named 'pName' (see NormalizeModuleName()), which QuickJS-ng has not loaded yet: fetched, compiled,
// its imports loaded. When it fails, at the root of a graph, its error is thrown (null returned), else it is recorded
// in the graph and the import resolves to a module that throws.
JSModuleDef* LoadModule(JSContext* pCtx, const char* pName, void* pPage, JSValueConst /*attributes*/) noexcept {
  Page& page = *static_cast<Page*>(pPage);
  ModuleMap& modules = page.modules();
  modules.countHookCall();
  try {
    if (std::string_view(pName) == ModuleMap::kFailedModuleName) {
      // Only nested: a failure at the root throws. Null on memory exhaustion, the exception pending.
      return JS_NewCModule(pCtx, pName, &ThrowFailedModule);
    }
    const Loaded loaded = FetchAndCompile(page, pName);
    if (loaded.pModule != nullptr) {
      return loaded.pModule;
    }

    JsValue error(pCtx, loaded.error);
    if (loaded.remember) {
      modules.addFailed(pName, error.get(), loaded.fetchFailure);
    }

    modules.fail(JS_DupValue(pCtx, error.get()), loaded.fetchFailure);

    // The modules that reach this one through a cycle stay loaded too: they fail with it.
    modules.failPending();
    if (modules.atRoot()) {
      JS_Throw(pCtx, error.release());
      return nullptr;
    }
    return JS_NewCModule(pCtx, ModuleMap::kFailedModuleName, &ThrowFailedModule);
  } catch (...) {
    JS_ThrowOutOfMemory(pCtx);
    return nullptr;
  }
}

// The own enumerable string keys of an object, freed with it.
class PropertyNames {
 public:
  // Check failed() first, the exception pending. The JS_GPN_* flags are distinct bits of signed int macros: added
  // rather than or-ed (bugprone-signed-bitwise).
  PropertyNames(JSContext* pCtx, JSValueConst object) noexcept
      : _pCtx(pCtx),
        _failed(JS_GetOwnPropertyNames(pCtx, &_pNames, &_size, object, JS_GPN_STRING_MASK + JS_GPN_ENUM_ONLY) < 0) {}

  PropertyNames(const PropertyNames&) = delete;
  PropertyNames& operator=(const PropertyNames&) = delete;
  PropertyNames(PropertyNames&&) = delete;
  PropertyNames& operator=(PropertyNames&&) = delete;

  ~PropertyNames() { JS_FreePropertyEnum(_pCtx, _pNames, _size); }  // nothing for a null pointer

  [[nodiscard]] bool failed() const noexcept { return _failed; }

  [[nodiscard]] std::span<const JSPropertyEnum> names() const noexcept { return {_pNames, _size}; }

 private:
  JSContext* _pCtx;
  JSPropertyEnum* _pNames{};
  uint32_t _size{};
  bool _failed;
};

// Refuses the import attributes, at the parsing of an import and at an import() call (ECMAScript
// "AllImportAttributesSupported", HTML "HostGetSupportedImportAttributes"): a key other than 'type' with a
// SyntaxError, and 'type' with a TypeError, as only JavaScript modules are supported (not JSON nor CSS ones). As no
// module loads with attributes, an import with none loads the same one as QuickJS-ng (it compares them). Returns -1,
// the error thrown, for any attribute.
int CheckImportAttributes(JSContext* pCtx, void* /*pPage*/, JSValueConst attributes) noexcept {
  // An object of strings made by QuickJS-ng (from the 'with' clause, or the 'with' option of import()): no getter runs.
  assert(JS_IsObject(attributes));
  const PropertyNames keys(pCtx, attributes);
  if (keys.failed()) {
    return -1;
  }
  if (keys.names().empty()) {
    return 0;
  }
  for (const JSPropertyEnum& key : keys.names()) {
    const JsValue keyValue(pCtx, JS_AtomToString(pCtx, key.atom));
    const JsString keyText(pCtx, keyValue.get());  // fails for JS_EXCEPTION too
    if (keyText.failed()) {
      return -1;
    }
    if (keyText.view() != "type") {
      JS_ThrowSyntaxError(pCtx, "Import attribute has an unsupported key '%.*s'",
                          static_cast<int>(keyText.view().size()), keyText.view().data());
      return -1;
    }
  }
  const JsValue type(pCtx, JS_GetPropertyStr(pCtx, attributes, "type"));
  const JsString typeText(pCtx, type.get());
  if (!typeText.failed()) {
    JS_ThrowTypeError(pCtx, "aeronet: the modules of type '%.*s' are not supported (only JavaScript modules are)",
                      static_cast<int>(typeText.view().size()), typeText.view().data());
  }
  return -1;
}

// JS_LoadModule() of the module 'pName' (an absolute URL), with the import attributes it takes since QuickJS-ng 0.17.
template <class Context>
JSValue LoadRootModule(Context* pCtx, const char* pName) noexcept {
  if constexpr (requires { JS_LoadModule(pCtx, pName, pName, JS_UNDEFINED); }) {
    return JS_LoadModule(pCtx, pName, pName, JS_UNDEFINED);
  } else {
    return JS_LoadModule(pCtx, pName, pName);
  }
}

// Reports 'reason', the error of a module script, unless the deadline stopped it.
void ReportModuleError(Page& page, JSValueConst reason) {
  if (!page.expired()) {
    page.reportError(ExceptionText(page.context(), reason));
  }
}

// The handler of the rejection of the evaluation of a module script (top-level await).
JSValue ReportEvaluationError(JSContext* pCtx, JSValueConst /*self*/, int argc, JSValueConst* argv) {
  ReportModuleError(Page::Of(pCtx), argc > 0 ? argv[0] : JS_UNDEFINED);
  return JS_UNDEFINED;
}

// Reports the error of 'promise', the evaluation of a module script: at once when it is rejected, when it rejects
// otherwise. Returns false on memory exhaustion, the exception pending.
bool WatchEvaluation(Page& page, JSValueConst promise) {
  JSContext* pCtx = page.context();
  switch (JS_PromiseState(pCtx, promise)) {
    case JS_PROMISE_REJECTED: {
      page.forgetRejection(promise);
      const JsValue reason(pCtx, JS_PromiseResult(pCtx, promise));
      ReportModuleError(page, reason.get());
      return true;
    }
    case JS_PROMISE_PENDING: {
      const JsValue onRejected(pCtx, JS_NewCFunction(pCtx, &JsMethod<ReportEvaluationError>, "", 1));
      if (JS_IsException(onRejected.get())) {
        return false;
      }
      const JsValue reaction(pCtx, JS_PromiseThen(pCtx, promise, JS_UNDEFINED, onRejected.get()));
      return !JS_IsException(reaction.get());
    }
    default:
      return true;
  }
}

// The outcome of a module script whose graph failed with 'error' (see ModuleMap::fail()).
ModuleRun GraphFailed(Page& page, JSValueConst error, bool fetchFailure) {
  if (fetchFailure) {
    return ModuleRun::FetchFailed;  // logged by FetchError()
  }
  ReportModuleError(page, error);
  return ModuleRun::Failed;
}

}  // namespace

bool ModulesEnabled() noexcept {
#ifdef AERONET_WEB_MODULES
  return true;
#else
  return false;
#endif
}

void InstallModuleLoader(Page& page) noexcept {
  JS_SetModuleLoaderFunc2(JS_GetRuntime(page.context()), &NormalizeModuleName, &LoadModule, &CheckImportAttributes,
                          &page);
}

ModuleRun RunExternalModule(Page& page, std::string_view url, RequestCredentials credentials) {
  JSContext* pCtx = page.context();
  ModuleMap& modules = page.modules();
  JsValue promise;
  {
    const ModuleMap::ScriptGraph graph(modules, credentials);
    // Null-terminated, in a buffer that the load does not use: 'url' can view the buffer of the URL resolver.
    promise = JsValue(pCtx, LoadRootModule(pCtx, modules.terminated(url)));
  }
  if (JS_IsException(promise.get())) {
    page.reportPendingException("module script");
    return ModuleRun::Failed;
  }
  // Rejected at once: the graph did not load or link. Its evaluation rejects later, through a reaction.
  if (JS_PromiseState(pCtx, promise.get()) == JS_PROMISE_REJECTED) {
    page.forgetRejection(promise.get());
    const JsValue reason(pCtx, JS_PromiseResult(pCtx, promise.get()));
    return GraphFailed(page, reason.get(), modules.graphFetchFailed());
  }
  if (!WatchEvaluation(page, promise.get())) {
    page.reportPendingException("module script");
  }
  return ModuleRun::Evaluated;
}

ModuleRun RunInlineModule(Page& page, std::string_view code, RequestCredentials credentials) {
  JSContext* pCtx = page.context();
  ModuleMap& modules = page.modules();
  if (modules.broken()) {
    log::debug("[js {}] inline module script not run: no module loads any more", page.url());
    return ModuleRun::FetchFailed;
  }

  const uint32_t hookCalls = modules.hookCalls();
  const uint32_t failures = modules.failures();

  JsValue module;
  {
    const ModuleMap::ScriptGraph graph(modules, credentials);
    // Named after its document: its imports resolve against the base URL of the document (see BaseUrlOf()).
    ModuleMap::Compilation compilation(modules, page.url());
    module = CompileModule(page, page.urlCStr(), code);
    if (!JS_IsException(module.get()) && modules.failures() == failures) {
      compilation.loaded();
    }
  }

  if (JS_IsException(module.get())) {
    if (modules.hookCalls() != hookCalls) {
      modules.setBroken();
    }
    page.reportPendingException("module script");
    return ModuleRun::Failed;
  }

  const bool graphFailed = modules.failures() != failures;
  if (graphFailed) {
    // The modules that reach it through a cycle fail with it (but its own name is that of its document).
    modules.failPending();
  }

  // Also when its graph failed: the inline modules of a document share its URL as name, and QuickJS-ng gives each of
  // them the import.meta of the first one loaded (js_import_meta() looks the module up by name), which stays loaded.
  if (!SetImportMeta(pCtx, static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(module.get())), page.baseUrl())) {
    page.reportPendingException("module script");
    return ModuleRun::Failed;
  }

  if (graphFailed) {
    return GraphFailed(page, modules.graphError(), modules.graphFetchFailed());
  }

  // Linked, then evaluated: a link error (an import that its module does not export) throws.
  const JsValue promise(pCtx, JS_EvalFunction(pCtx, module.release()));
  if (JS_IsException(promise.get()) || !WatchEvaluation(page, promise.get())) {
    page.reportPendingException("module script");
    return ModuleRun::Failed;
  }
  return ModuleRun::Evaluated;
}

}  // namespace aeronet::internal
