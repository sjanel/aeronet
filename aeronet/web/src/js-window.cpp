// The window of a page: location, navigator, storage, timers, and the native helpers of the prelude.

#include "js-window.hpp"

#include <quickjs.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/http-client-config.hpp"
#include "aeronet/internal/web-session.hpp"
#include "aeronet/log-noexcept.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/web-client-config.hpp"
#include "js-engine.hpp"
#include "page.hpp"
#include "web-url.hpp"

namespace aeronet::internal {

namespace {

// The JS_PROP_* flags are distinct bits of signed int macros: added rather than or-ed (bugprone-signed-bitwise).
constexpr int kEnumerableWritable = JS_PROP_ENUMERABLE + JS_PROP_WRITABLE;
constexpr int kElement = JS_PROP_CONFIGURABLE + JS_PROP_WRITABLE + JS_PROP_ENUMERABLE;  // of an array

// --- Location ---

JSValue GetHref(JSContext* pCtx, JSValueConst /*self*/) { return NewString(pCtx, Page::Of(pCtx).url()); }

JSValue SetHref(JSContext* pCtx, JSValueConst /*self*/, JSValueConst value) {
  const JsString url(pCtx, value);
  return !url.failed() && Page::Of(pCtx).navigateTo(url.view()) ? JS_UNDEFINED : JS_EXCEPTION;
}

template <std::string_view UrlParts::* kPart>
JSValue GetUrlPart(JSContext* pCtx, JSValueConst /*self*/) {
  return NewString(pCtx, SplitUrl(Page::Of(pCtx).url()).*kPart);
}

JSValue GetOrigin(JSContext* pCtx, JSValueConst /*self*/) { return NewString(pCtx, Page::Of(pCtx).origin()); }

// The value of a setter of a URL part, without the leading 'prefix' character ('?', '#') if present.
std::string_view WithoutPrefix(std::string_view value, char prefix) noexcept {
  return value.starts_with(prefix) ? value.substr(1) : value;
}

JSValue SetHash(JSContext* pCtx, JSValueConst /*self*/, JSValueConst value) {
  const JsString hash(pCtx, value);
  if (hash.failed()) {
    return JS_EXCEPTION;
  }
  Page& page = Page::Of(pCtx);
  return page.navigateTo({WithoutFragment(page.url()), "#", WithoutPrefix(hash.view(), '#')}) ? JS_UNDEFINED
                                                                                              : JS_EXCEPTION;
}

JSValue SetSearch(JSContext* pCtx, JSValueConst /*self*/, JSValueConst value) {
  const JsString search(pCtx, value);
  if (search.failed()) {
    return JS_EXCEPTION;
  }
  Page& page = Page::Of(pCtx);
  const std::string_view query = WithoutPrefix(search.view(), '?');
  const std::string_view questionMark = query.empty() ? std::string_view{} : std::string_view("?");
  const UrlParts parts = SplitUrl(page.url());

  return page.navigateTo({page.origin(), parts.pathname, questionMark, query, parts.hash}) ? JS_UNDEFINED
                                                                                           : JS_EXCEPTION;
}

JSValue SetPathname(JSContext* pCtx, JSValueConst /*self*/, JSValueConst value) {
  const JsString pathname(pCtx, value);
  if (pathname.failed()) {
    return JS_EXCEPTION;
  }
  Page& page = Page::Of(pCtx);
  const UrlParts parts = SplitUrl(page.url());
  const std::string_view path = pathname.view();

  return page.navigateTo({
             page.origin(),
             path.starts_with('/') ? std::string_view{} : std::string_view("/"),
             path,
             parts.search,
             parts.hash,
         })
             ? JS_UNDEFINED
             : JS_EXCEPTION;
}

JSValue Assign(JSContext* pCtx, JSValueConst /*self*/, int argc, JSValueConst* argv) {
  const JsArgument url(pCtx, argc, argv, 0);
  return !url.failed() && Page::Of(pCtx).navigateTo(url.view()) ? JS_UNDEFINED : JS_EXCEPTION;
}

JSValue Reload(JSContext* pCtx, JSValueConst /*self*/, int /*argc*/, JSValueConst* /*argv*/) {
  Page::Of(pCtx).reload();
  return JS_UNDEFINED;
}

JSValue LocationToString(JSContext* pCtx, JSValueConst self, int /*argc*/, JSValueConst* /*argv*/) {
  return GetHref(pCtx, self);
}

const JSCFunctionListEntry kLocationFunctions[] = {
    JS_CGETSET_DEF("href", &JsGetter<GetHref>, &JsSetter<SetHref>),
    JS_CGETSET_DEF("origin", &JsGetter<GetOrigin>, nullptr),
    JS_CGETSET_DEF("protocol", &JsGetter<GetUrlPart<&UrlParts::protocol>>, nullptr),
    JS_CGETSET_DEF("host", &JsGetter<GetUrlPart<&UrlParts::host>>, nullptr),
    JS_CGETSET_DEF("hostname", &JsGetter<GetUrlPart<&UrlParts::hostname>>, nullptr),
    JS_CGETSET_DEF("port", &JsGetter<GetUrlPart<&UrlParts::port>>, nullptr),
    JS_CGETSET_DEF("pathname", &JsGetter<GetUrlPart<&UrlParts::pathname>>, &JsSetter<SetPathname>),
    JS_CGETSET_DEF("search", &JsGetter<GetUrlPart<&UrlParts::search>>, &JsSetter<SetSearch>),
    JS_CGETSET_DEF("hash", &JsGetter<GetUrlPart<&UrlParts::hash>>, &JsSetter<SetHash>),
    JS_CFUNC_DEF("assign", 1, &JsMethod<Assign>),
    JS_CFUNC_DEF("replace", 1, &JsMethod<Assign>),
    JS_CFUNC_DEF("reload", 0, &JsMethod<Reload>),
    JS_CFUNC_DEF("toString", 0, &JsMethod<LocationToString>),
};

// --- Storage ---

// The area of the page in the storage of 'self' (localStorage or sessionStorage): null, with the TypeError of an
// illegal invocation thrown, when 'self' is not one of them.
StorageAreas::Area* StorageOf(JSContext* pCtx, JSValueConst self) {
  Page& page = Page::Of(pCtx);
  JSClassID classId = 0;
  void* pAreas = JS_GetAnyOpaque(self, &classId);
  if (pAreas != &page.session().localStorage && pAreas != &page.session().sessionStorage) {
    ThrowIllegalInvocation(pCtx);
    return nullptr;
  }
  return &page.storage(*static_cast<StorageAreas*>(pAreas));
}

auto FindItem(StorageAreas::Area& area, std::string_view key) noexcept {
  return std::ranges::find_if(area.items, [key](const StorageAreas::Item& item) { return item.key() == key; });
}

JSValue GetItem(JSContext* pCtx, JSValueConst self, int argc, JSValueConst* argv) {
  StorageAreas::Area* pArea = StorageOf(pCtx, self);
  if (pArea == nullptr) {
    return JS_EXCEPTION;
  }
  const JsArgument key(pCtx, argc, argv, 0);
  if (key.failed()) {
    return JS_EXCEPTION;
  }
  const auto itemIt = FindItem(*pArea, key.view());
  return itemIt == pArea->items.end() ? JS_NULL : NewString(pCtx, itemIt->value());
}

JSValue SetItem(JSContext* pCtx, JSValueConst self, int argc, JSValueConst* argv) {
  StorageAreas::Area* pArea = StorageOf(pCtx, self);
  if (pArea == nullptr) {
    return JS_EXCEPTION;
  }
  const JsArgument key(pCtx, argc, argv, 0);
  if (key.failed()) {
    return JS_EXCEPTION;
  }
  const JsArgument value(pCtx, argc, argv, 1);
  if (value.failed()) {
    return JS_EXCEPTION;
  }
  StorageAreas::Area& area = *pArea;
  const auto itemIt = FindItem(area, key.view());
  const std::size_t oldBytes = itemIt == area.items.end() ? 0 : itemIt->key().size() + itemIt->value().size();
  const std::size_t bytes = area.bytes - oldBytes + key.view().size() + value.view().size();
  if (bytes > StorageAreas::kMaxBytesPerOrigin) {
    return JS_ThrowDOMException(pCtx, "QuotaExceededError", "The quota of the storage of this origin is exceeded");
  }
  if (itemIt == area.items.end()) {
    area.items.emplace_back(key.view(), value.view());
  } else {
    itemIt->setValue(value.view());
  }
  area.bytes = static_cast<uint32_t>(bytes);
  return JS_UNDEFINED;
}

JSValue RemoveItem(JSContext* pCtx, JSValueConst self, int argc, JSValueConst* argv) {
  StorageAreas::Area* pArea = StorageOf(pCtx, self);
  if (pArea == nullptr) {
    return JS_EXCEPTION;
  }
  const JsArgument key(pCtx, argc, argv, 0);
  if (key.failed()) {
    return JS_EXCEPTION;
  }
  const auto itemIt = FindItem(*pArea, key.view());
  if (itemIt != pArea->items.end()) {
    pArea->bytes -= static_cast<uint32_t>(itemIt->key().size() + itemIt->value().size());
    pArea->items.erase(itemIt);
  }
  return JS_UNDEFINED;
}

JSValue Clear(JSContext* pCtx, JSValueConst self, int /*argc*/, JSValueConst* /*argv*/) {
  StorageAreas::Area* pArea = StorageOf(pCtx, self);
  if (pArea == nullptr) {
    return JS_EXCEPTION;
  }
  pArea->items.clear();
  pArea->bytes = 0;
  return JS_UNDEFINED;
}

JSValue Key(JSContext* pCtx, JSValueConst self, int argc, JSValueConst* argv) {
  const StorageAreas::Area* pArea = StorageOf(pCtx, self);
  int64_t index = 0;
  if (pArea == nullptr || (argc > 0 && JS_ToInt64(pCtx, &index, argv[0]) < 0)) {
    return JS_EXCEPTION;
  }
  return index >= 0 && std::cmp_less(index, pArea->items.size())
             ? NewString(pCtx, pArea->items[static_cast<vector<StorageAreas::Item>::size_type>(index)].key())
             : JS_NULL;
}

JSValue GetStorageLength(JSContext* pCtx, JSValueConst self) {
  const StorageAreas::Area* pArea = StorageOf(pCtx, self);
  return pArea == nullptr ? JS_EXCEPTION : JS_NewInt64(pCtx, static_cast<int64_t>(pArea->items.size()));
}

const JSCFunctionListEntry kStorageFunctions[] = {
    JS_CGETSET_DEF("length", &JsGetter<GetStorageLength>, nullptr),
    JS_CFUNC_DEF("getItem", 1, &JsMethod<GetItem>),
    JS_CFUNC_DEF("setItem", 2, &JsMethod<SetItem>),
    JS_CFUNC_DEF("removeItem", 1, &JsMethod<RemoveItem>),
    JS_CFUNC_DEF("clear", 0, &JsMethod<Clear>),
    JS_CFUNC_DEF("key", 1, &JsMethod<Key>),
};

// --- Timers ---

template <bool kRepeat>
JSValue SetTimer(JSContext* pCtx, JSValueConst /*self*/, int argc, JSValueConst* argv) {
  if (argc == 0) {
    return JS_ThrowTypeError(pCtx, "%s: 1 argument required", kRepeat ? "setInterval" : "setTimeout");
  }
  // A handler that is not a function is the code of a script.
  JsValue code;
  JSValueConst callback = argv[0];
  if (!JS_IsFunction(pCtx, callback)) {
    code = JsValue(pCtx, JS_ToString(pCtx, callback));
    if (JS_IsException(code.get())) {
      return JS_EXCEPTION;
    }
    callback = code.get();
  }
  int32_t delay = 0;
  if (argc > 1 && JS_ToInt32(pCtx, &delay, argv[1]) < 0) {
    return JS_EXCEPTION;
  }
  const std::span<const JSValueConst> args(argv + std::min(argc, 2), argv + argc);
  const int32_t id = Page::Of(pCtx).timers().add(callback, args, delay, kRepeat);
  return id == 0 ? JS_ThrowOutOfMemory(pCtx) : JS_NewInt32(pCtx, id);
}

JSValue ClearTimer(JSContext* pCtx, JSValueConst /*self*/, int argc, JSValueConst* argv) {
  int32_t id = 0;
  if (argc > 0 && JS_ToInt32(pCtx, &id, argv[0]) < 0) {
    return JS_EXCEPTION;
  }
  Page::Of(pCtx).timers().clear(id);
  return JS_UNDEFINED;
}

// --- Window ---

JSValue GetLocation(JSContext* pCtx, JSValueConst /*self*/) { return JS_DupValue(pCtx, Page::Of(pCtx).location()); }

const JSCFunctionListEntry kWindowFunctions[] = {
    JS_CGETSET_DEF("location", &JsGetter<GetLocation>, &JsSetter<SetHref>),
    JS_CGETSET_DEF("origin", &JsGetter<GetOrigin>, nullptr),
    JS_CFUNC_DEF("setTimeout", 1, &JsMethod<SetTimer<false>>),
    JS_CFUNC_DEF("setInterval", 1, &JsMethod<SetTimer<true>>),
    JS_CFUNC_DEF("clearTimeout", 0, &JsMethod<ClearTimer>),
    JS_CFUNC_DEF("clearInterval", 0, &JsMethod<ClearTimer>),
};

// --- Native helpers of the prelude ---

JSValue Log(JSContext* pCtx, JSValueConst /*self*/, int argc, JSValueConst* argv) {
  const JsArgument level(pCtx, argc, argv, 0);
  if (level.failed()) {
    return JS_EXCEPTION;
  }
  const JsArgument text(pCtx, argc, argv, 1);
  if (text.failed()) {
    return JS_EXCEPTION;
  }
  log_noexcept::debug("[js {}] {}: {}", Page::Of(pCtx).url(), level.view(), text.view());
  return JS_UNDEFINED;
}

JSValue ReportError(JSContext* pCtx, JSValueConst /*self*/, int argc, JSValueConst* argv) {
  Page::Of(pCtx).reportError(ExceptionText(pCtx, argc > 0 ? argv[0] : JS_UNDEFINED));
  return JS_UNDEFINED;
}

JSValue SetUrl(JSContext* pCtx, JSValueConst /*self*/, int argc, JSValueConst* argv) {
  const JsArgument url(pCtx, argc, argv, 0);
  return !url.failed() && Page::Of(pCtx).replaceUrl(url.view()) ? JS_UNDEFINED : JS_EXCEPTION;
}

JSValue ResolveUrlNative(JSContext* pCtx, JSValueConst /*self*/, int argc, JSValueConst* argv) {
  const JsArgument url(pCtx, argc, argv, 0);
  return url.failed() ? JS_EXCEPTION : NewString(pCtx, Page::Of(pCtx).resolveUrl(url.view()));
}

const JSCFunctionListEntry kPreludeNatives[] = {
    JS_CFUNC_DEF("log", 2, &JsMethod<Log>),
    JS_CFUNC_DEF("reportError", 1, &JsMethod<ReportError>),
    JS_CFUNC_DEF("setUrl", 1, &JsMethod<SetUrl>),
    JS_CFUNC_DEF("resolveUrl", 1, &JsMethod<ResolveUrlNative>),
};

// --- Installation ---

// The value of the first header 'name' of the global headers of 'config', empty if absent.
std::string_view GlobalHeader(const HttpClientConfig& config, std::string_view name) noexcept {
  for (const std::string_view header : config.globalHeaders) {
    const std::size_t colon = header.find(':');
    if (colon != std::string_view::npos && CaseInsensitiveEqual(header.substr(0, colon), name)) {
      return TrimOws(header.substr(colon + 1));
    }
  }
  return {};
}

// navigator.language: the first language of the Accept-Language header, else "en-US".
std::string_view FirstLanguage(std::string_view acceptLanguage) noexcept {
  const std::string_view language = TrimOws(acceptLanguage.substr(0, acceptLanguage.find_first_of(",;")));
  return language.empty() ? std::string_view("en-US") : language;
}

struct NavigatorString {
  const char* name;
  std::string_view value;
};

// The navigator object of a page, owned, or JS_EXCEPTION on memory exhaustion.
JSValue NewNavigator(JSContext* pCtx, const WebClientConfig& config) {
  JsValue navigator(pCtx, JS_NewObject(pCtx));
  if (JS_IsException(navigator.get())) {
    return JS_EXCEPTION;
  }
  // The User-Agent header of the requests.
  const std::string_view userAgent = GlobalHeader(config.http, "user-agent");
  const std::string_view appVersion =
      userAgent.starts_with("Mozilla/") ? userAgent.substr(std::string_view("Mozilla/").size()) : userAgent;
  const std::string_view language = FirstLanguage(GlobalHeader(config.http, "accept-language"));
  const NavigatorString strings[] = {
      {"userAgent", userAgent}, {"appVersion", appVersion}, {"appName", "Netscape"}, {"appCodeName", "Mozilla"},
      {"product", "Gecko"},     {"platform", ""},           {"vendor", ""},          {"language", language},
  };
  for (const NavigatorString& string : strings) {
    if (!Define(pCtx, navigator.get(), string.name, NewString(pCtx, string.value), JS_PROP_ENUMERABLE)) {
      return JS_EXCEPTION;
    }
  }
  JsValue languages(pCtx, JS_NewArray(pCtx));
  if (JS_IsException(languages.get()) || !Define(pCtx, languages.get(), "0", NewString(pCtx, language), kElement) ||
      !Define(pCtx, navigator.get(), "languages", languages.release(), JS_PROP_ENUMERABLE) ||
      !Define(pCtx, navigator.get(), "cookieEnabled", JS_TRUE, JS_PROP_ENUMERABLE) ||
      !Define(pCtx, navigator.get(), "onLine", JS_TRUE, JS_PROP_ENUMERABLE) ||
      !Define(pCtx, navigator.get(), "hardwareConcurrency", JS_NewInt32(pCtx, 1), JS_PROP_ENUMERABLE) ||
      !Define(pCtx, navigator.get(), "maxTouchPoints", JS_NewInt32(pCtx, 0), JS_PROP_ENUMERABLE)) {
    return JS_EXCEPTION;
  }
  return navigator.release();
}

struct NamedStorage {
  const char* name;
  StorageAreas* pAreas;
};

}  // namespace

JSValue InstallWindow(Page& page) {
  JSContext* pCtx = page.context();
  const JsValue global(pCtx, JS_GetGlobalObject(pCtx));
  for (const char* name : {"window", "self", "frames", "parent", "top"}) {
    if (!Define(pCtx, global.get(), name, JS_DupValue(pCtx, global.get()), JS_PROP_ENUMERABLE)) {
      return JS_EXCEPTION;
    }
  }
  if (!Define(pCtx, global.get(), "name", NewString(pCtx, {}), kEnumerableWritable) ||
      !Define(pCtx, global.get(), "opener", JS_NULL, kEnumerableWritable) ||
      !Define(pCtx, global.get(), "closed", JS_FALSE, JS_PROP_ENUMERABLE) ||
      !SetFunctions(pCtx, global.get(), kWindowFunctions)) {
    return JS_EXCEPTION;
  }

  JsValue location(pCtx, JS_NewObject(pCtx));
  if (JS_IsException(location.get()) || !SetFunctions(pCtx, location.get(), kLocationFunctions)) {
    return JS_EXCEPTION;
  }
  page.setLocation(std::move(location));

  WebSession& session = page.session();
  if (!Define(pCtx, global.get(), "navigator", NewNavigator(pCtx, session.config), JS_PROP_ENUMERABLE)) {
    return JS_EXCEPTION;
  }

  // Storage: one class, whose objects point to the StorageAreas of the session.
  const JSClassID storageClass = NewClass(pCtx, "Storage");
  if (storageClass == 0) {
    return JS_EXCEPTION;
  }
  JsValue storagePrototype(pCtx, JS_NewObject(pCtx));
  if (JS_IsException(storagePrototype.get()) || !SetFunctions(pCtx, storagePrototype.get(), kStorageFunctions)) {
    return JS_EXCEPTION;
  }
  JS_SetClassProto(pCtx, storageClass, storagePrototype.release());
  for (const NamedStorage storage : {
           NamedStorage{"localStorage", &session.localStorage},
           NamedStorage{"sessionStorage", &session.sessionStorage},
       }) {
    JsValue object(pCtx, JS_NewObjectClass(pCtx, storageClass));
    if (JS_IsException(object.get())) {
      return JS_EXCEPTION;
    }
    SetOpaque(object.get(), storage.pAreas);
    if (!Define(pCtx, global.get(), storage.name, object.release(), JS_PROP_ENUMERABLE)) {
      return JS_EXCEPTION;
    }
  }

  JsValue natives(pCtx, JS_NewObject(pCtx));
  if (JS_IsException(natives.get()) || !SetFunctions(pCtx, natives.get(), kPreludeNatives)) {
    return JS_EXCEPTION;
  }
  return natives.release();
}

}  // namespace aeronet::internal
