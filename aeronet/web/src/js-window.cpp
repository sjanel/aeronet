// The window of a page: location, navigator, storage, timers, and the native helpers of the prelude.

#include "js-window.hpp"

#include <quickjs.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/cctype.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/internal/web-session.hpp"
#include "aeronet/log-noexcept.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/toupperlower.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/web-client-config.hpp"
#include "js-dom.hpp"
#include "js-engine.hpp"
#include "js-prelude.hpp"
#include "page.hpp"
#include "web-url.hpp"

namespace aeronet::internal {

namespace {

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

const JSCFunctionListEntry kLocationFunctions[]{
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

const JSCFunctionListEntry kStorageFunctions[]{
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

JSValue Noop(JSContext* /*pCtx*/, JSValueConst /*self*/, int /*argc*/, JSValueConst* /*argv*/) { return JS_UNDEFINED; }

// The viewport of a desktop browser, without layout: the elements have no size (see js-dom.cpp).
constexpr int32_t kViewportWidth = 1280;
constexpr int32_t kViewportHeight = 720;

// The [Replaceable] attributes of the window: a script can assign them.
constexpr int kReplaceable = kOrdinaryProperty;

// The orientation never changes: no change event to deliver, its listeners are not kept.
const JSCFunctionListEntry kScreenOrientation[]{
    JS_PROP_STRING_DEF("type", "landscape-primary", JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("angle", 0, JS_PROP_ENUMERABLE),
    JS_CFUNC_DEF("addEventListener", 2, &Noop),
    JS_CFUNC_DEF("removeEventListener", 2, &Noop),
};

const JSCFunctionListEntry kScreen[]{
    JS_PROP_INT32_DEF("width", kViewportWidth, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("height", kViewportHeight, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("availWidth", kViewportWidth, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("availHeight", kViewportHeight, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("availLeft", 0, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("availTop", 0, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("colorDepth", 24, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("pixelDepth", 24, JS_PROP_ENUMERABLE),
    JS_OBJECT_DEF("orientation", kScreenOrientation, std::size(kScreenOrientation), JS_PROP_ENUMERABLE),
};

// The values and the native methods cost nothing to set up: QuickJS-ng creates a method on its first use, as the
// 'screen' object.
const JSCFunctionListEntry kWindowFunctions[]{
    JS_CGETSET_DEF("location", &JsGetter<GetLocation>, &JsSetter<SetHref>),
    JS_CGETSET_DEF("origin", &JsGetter<GetOrigin>, nullptr),
    JS_CFUNC_DEF("setTimeout", 1, &JsMethod<SetTimer<false>>),
    JS_CFUNC_DEF("setInterval", 1, &JsMethod<SetTimer<true>>),
    JS_CFUNC_DEF("clearTimeout", 0, &JsMethod<ClearTimer>),
    JS_CFUNC_DEF("clearInterval", 0, &JsMethod<ClearTimer>),
    JS_CFUNC_DEF("scroll", 0, &Noop),
    JS_CFUNC_DEF("scrollTo", 0, &Noop),
    JS_CFUNC_DEF("scrollBy", 0, &Noop),
    JS_PROP_INT32_DEF("innerWidth", kViewportWidth, kReplaceable),
    JS_PROP_INT32_DEF("innerHeight", kViewportHeight, kReplaceable),
    JS_PROP_INT32_DEF("outerWidth", kViewportWidth, kReplaceable),
    JS_PROP_INT32_DEF("outerHeight", kViewportHeight, kReplaceable),
    JS_PROP_INT32_DEF("devicePixelRatio", 1, kReplaceable),
    JS_PROP_INT32_DEF("scrollX", 0, kReplaceable),
    JS_PROP_INT32_DEF("scrollY", 0, kReplaceable),
    JS_PROP_INT32_DEF("pageXOffset", 0, kReplaceable),
    JS_PROP_INT32_DEF("pageYOffset", 0, kReplaceable),
    JS_PROP_INT32_DEF("screenX", 0, kReplaceable),
    JS_PROP_INT32_DEF("screenY", 0, kReplaceable),
    JS_PROP_INT32_DEF("screenLeft", 0, kReplaceable),
    JS_PROP_INT32_DEF("screenTop", 0, kReplaceable),
    JS_OBJECT_DEF("screen", kScreen, std::size(kScreen), kReplaceable),
};

// --- Lazy parts of the prelude ---

// A method that calls the function 'name' of the lazy part 'part' of the prelude, run on its first use: the methods of
// the built-ins that format with Intl, navigator.sendBeacon().
struct LazyMethod {
  LazyPrelude part;
  const char* name;
};

constexpr LazyMethod kLazyMethods[]{
    {LazyPrelude::Network, "sendBeacon"},          {LazyPrelude::Intl, "numberToLocaleString"},
    {LazyPrelude::Intl, "localeCompare"},          {LazyPrelude::Intl, "dateToLocaleString"},
    {LazyPrelude::Intl, "dateToLocaleDateString"}, {LazyPrelude::Intl, "dateToLocaleTimeString"},
};

template <std::size_t kIndex>
JSValue CallLazyMethod(JSContext* pCtx, JSValueConst self, int argc, JSValueConst* argv) {
  const JSValue part = Page::Of(pCtx).lazyPart(kLazyMethods[kIndex].part);
  if (JS_IsException(part)) {
    return JS_EXCEPTION;
  }
  const JsValue function(pCtx, JS_GetPropertyStr(pCtx, part, kLazyMethods[kIndex].name));
  return JS_IsException(function.get()) ? JS_EXCEPTION : JS_Call(pCtx, function.get(), self, argc, argv);
}

bool IsAscii(std::string_view str) noexcept {
  return std::ranges::all_of(str, [](char ch) { return static_cast<unsigned char>(ch) < 0x80U; });
}

// The order of the Collator of the Intl part for ASCII strings: the letters without their case first (as lower case),
// then the first difference of case, lower case first.
int CompareAscii(std::string_view lhs, std::string_view rhs) noexcept {
  const std::size_t size = std::min(lhs.size(), rhs.size());
  for (std::size_t pos = 0; pos < size; ++pos) {
    const char left = tolower(lhs[pos]);
    const char right = tolower(rhs[pos]);
    if (left != right) {
      return left < right ? -1 : 1;
    }
  }
  if (lhs.size() != rhs.size()) {
    return lhs.size() < rhs.size() ? -1 : 1;
  }
  const auto [left, right] = std::ranges::mismatch(lhs, rhs);
  if (left == lhs.end()) {
    return 0;
  }
  return islower(*left) ? -1 : 1;
}

// String.prototype.localeCompare(that, locales, options): the Collator of the Intl part (en-US), with a native path for
// two ASCII strings compared without locales nor options, the common case of the sorts: the same order, without the
// setup of the Intl part (200 us) nor its compare() (about 50 times slower). QuickJS-ng compares the code points.
JSValue LocaleCompare(JSContext* pCtx, JSValueConst self, int argc, JSValueConst* argv) {
  const bool withOptions = (argc > 1 && !JS_IsUndefined(argv[1])) || (argc > 2 && !JS_IsUndefined(argv[2]));
  if (!withOptions && argc > 0 && JS_IsString(self) && JS_IsString(argv[0])) {
    // Not copied when ASCII.
    const JsString lhs(pCtx, self);
    const JsString rhs(pCtx, argv[0]);
    if (lhs.failed() || rhs.failed()) {
      return JS_EXCEPTION;
    }
    if (IsAscii(lhs.view()) && IsAscii(rhs.view())) {
      return JS_NewInt32(pCtx, CompareAscii(lhs.view(), rhs.view()));
    }
  }
  return CallLazyMethod<2>(pCtx, self, argc, argv);
}

// A method replacing one of a built-in prototype: defined at once, as function lists (created on first use) cannot
// replace a property (QuickJS-ng aborts).
struct BuiltInMethod {
  const char* constructor;
  const char* name;
  int length;
  JSCFunction* function;
};

constexpr BuiltInMethod kIntlMethods[]{
    {"Number", "toLocaleString", 0, &JsMethod<CallLazyMethod<1>>},
    {"String", "localeCompare", 1, &JsMethod<LocaleCompare>},
    {"Date", "toLocaleString", 0, &JsMethod<CallLazyMethod<3>>},
    {"Date", "toLocaleDateString", 0, &JsMethod<CallLazyMethod<4>>},
    {"Date", "toLocaleTimeString", 0, &JsMethod<CallLazyMethod<5>>},
};

const JSCFunctionListEntry kNavigatorMethods[]{
    JS_CFUNC_DEF("sendBeacon", 1, &JsMethod<CallLazyMethod<0>>),
};

// The globals of the lazy parts of the prelude, sorted by name.
struct LazyGlobal {
  std::string_view name;
  LazyPrelude part;
};

constexpr LazyGlobal kLazyGlobals[]{
    {"AbortController", LazyPrelude::Network},
    {"AbortSignal", LazyPrelude::Network},
    {"DOMRect", LazyPrelude::View},
    {"DOMRectReadOnly", LazyPrelude::View},
    {"Headers", LazyPrelude::Network},
    {"IntersectionObserver", LazyPrelude::View},
    {"IntersectionObserverEntry", LazyPrelude::View},
    {"Intl", LazyPrelude::Intl},
    {"MediaQueryList", LazyPrelude::View},
    {"ProgressEvent", LazyPrelude::Network},
    {"Request", LazyPrelude::Network},
    {"ResizeObserver", LazyPrelude::View},
    {"ResizeObserverEntry", LazyPrelude::View},
    {"Response", LazyPrelude::Network},
    {"TextDecoder", LazyPrelude::Network},
    {"TextEncoder", LazyPrelude::Network},
    {"URL", LazyPrelude::Network},
    {"URLSearchParams", LazyPrelude::Network},
    {"XMLHttpRequest", LazyPrelude::Network},
    {"XMLHttpRequestEventTarget", LazyPrelude::Network},
    {"XMLHttpRequestUpload", LazyPrelude::Network},
    {"fetch", LazyPrelude::Network},
    {"getComputedStyle", LazyPrelude::View},
    {"matchMedia", LazyPrelude::View},
};

static_assert(std::ranges::is_sorted(kLazyGlobals, {}, &LazyGlobal::name));

constexpr int kNbLazyGlobals = static_cast<int>(std::size(kLazyGlobals));

// The names of the HTML element interfaces: HTML...Element.
constexpr std::string_view kHtmlInterfacePrefix = "HTML";
constexpr std::string_view kHtmlInterfaceSuffix = "Element";

// The index of the lazy global 'name': that of kLazyGlobals, or kNbLazyGlobals plus that of an HTML element interface,
// -1 if it is not one. Most lookups are of other names (feature detection, globals of the page): rejected after a
// binary search and a prefix check.
int LazyGlobalIndex(std::string_view name) noexcept {
  const auto* pGlobal = std::ranges::lower_bound(kLazyGlobals, name, {}, &LazyGlobal::name);
  if (pGlobal != std::end(kLazyGlobals) && pGlobal->name == name) {
    return static_cast<int>(pGlobal - std::begin(kLazyGlobals));
  }
  if (!name.starts_with(kHtmlInterfacePrefix) || !name.ends_with(kHtmlInterfaceSuffix)) {
    return -1;
  }
  const std::size_t htmlInterface = FindHtmlInterface(name);
  return htmlInterface == kNbHtmlInterfaces ? -1 : kNbLazyGlobals + static_cast<int>(htmlInterface);
}

// The getter of the lazy global 'index' (see LazyGlobalIndex()), named 'name': sets it up (the lazy part of the
// prelude that defines it, or the HTML element interface), then defines its value on the object of the lazy globals,
// which the next lookups find without this getter.
JSValue LazyGlobalGetter(JSContext* pCtx, JSValueConst /*self*/, int /*argc*/, JSValueConst* /*argv*/, int index) {
  Page& page = Page::Of(pCtx);
  const JSValueConst lazyGlobals = page.lazyGlobals();
  JsValue value;
  const char* name = nullptr;
  if (index < kNbLazyGlobals) {
    const LazyGlobal& global = kLazyGlobals[index];
    const JSValue part = page.lazyPart(global.part);
    if (JS_IsException(part)) {
      return JS_EXCEPTION;
    }
    name = global.name.data();  // null-terminated literals
    value = JsValue(pCtx, JS_GetPropertyStr(pCtx, part, name));
  } else {
    const auto htmlInterface = static_cast<std::size_t>(index - kNbLazyGlobals);
    name = HtmlInterfaceName(htmlInterface);
    value = JsValue(pCtx, InstallHtmlInterface(page, htmlInterface));
  }
  if (JS_IsException(value.get()) ||
      JS_DefinePropertyValueStr(pCtx, lazyGlobals, name, JS_DupValue(pCtx, value.get()), kWritableConfigurable) < 0) {
    return JS_EXCEPTION;
  }
  return value.release();
}

// The setter of the lazy global 'index': an assignment defines its own property on the receiver (the window), as for
// any global, without the setup of the value that it replaces.
JSValue LazyGlobalSetter(JSContext* pCtx, JSValueConst self, int argc, JSValueConst* argv, int index) noexcept {
  const char* name = index < kNbLazyGlobals ? kLazyGlobals[index].name.data()
                                            : HtmlInterfaceName(static_cast<std::size_t>(index - kNbLazyGlobals));
  return JS_DefinePropertyValueStr(pCtx, self, name, JS_DupValue(pCtx, argc > 0 ? argv[0] : JS_UNDEFINED),
                                   kOrdinaryProperty) < 0
             ? JS_EXCEPTION
             : JS_UNDEFINED;
}

// The own property 'atom' of the object of the lazy globals, between the window and EventTarget.prototype: the lookup
// of a global that the window does not have. A global of the lazy parts of the prelude or an HTML element interface is
// an accessor (see LazyGlobalGetter() and LazyGlobalSetter()) until its first read, which defines it as an ordinary
// property of this object, which QuickJS-ng then finds before calling this function. Nothing is defined on the window:
// an assignment to the window looks its prototypes up for a setter, then adds the property to the window without
// checking it again (it would have it twice). An existence check ('in', pDesc null) creates nothing. Returns 1 when it
// is one, 0 when not, -1 on failure with the exception pending.
int GetLazyGlobal(JSContext* pCtx, JSPropertyDescriptor* pDesc, JSValueConst /*self*/, JSAtom atom) noexcept {
  int index = -1;
  {
    const JsValue key(pCtx, JS_AtomToValue(pCtx, atom));
    if (!JS_IsString(key.get())) {
      return JS_IsException(key.get()) ? -1 : 0;  // a symbol, or an integer index
    }
    const JsString name(pCtx, key.get());  // not copied: atoms of ASCII names
    if (name.failed()) {
      return -1;
    }
    index = LazyGlobalIndex(name.view());
  }
  if (index < 0) {
    return 0;
  }
  if (pDesc == nullptr) {
    return 1;
  }
  // Created on each lookup until the first read, as rare as the assignments before one.
  JsValue getter(pCtx,
                 JS_NewCFunctionMagic(pCtx, &JsMagicMethod<LazyGlobalGetter>, "get", 0, JS_CFUNC_generic_magic, index));
  if (JS_IsException(getter.get())) {
    return -1;
  }
  JsValue setter(pCtx, JS_NewCFunctionMagic(pCtx, &LazyGlobalSetter, "set", 1, JS_CFUNC_generic_magic, index));
  if (JS_IsException(setter.get())) {
    return -1;
  }
  pDesc->flags = JS_PROP_GETSET + JS_PROP_CONFIGURABLE;
  pDesc->value = JS_UNDEFINED;
  pDesc->getter = getter.release();
  pDesc->setter = setter.release();
  return 1;
}

// The other lookups (names, definitions...) are those of an ordinary object.
const JSClassExoticMethods kLazyGlobalsMethods{
    .get_own_property = &GetLazyGlobal,
    .get_own_property_names = nullptr,
    .delete_property = nullptr,
    .define_own_property = nullptr,
    .has_property = nullptr,
    .get_property = nullptr,
    .set_property = nullptr,
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
  Page& page = Page::Of(pCtx);
  const std::string text = ExceptionText(pCtx, argc > 0 ? argv[0] : JS_UNDEFINED);
  // The conversion of the error (its toString()) ran until the deadline, whose error ExceptionText() took: the script
  // that reports it stops too.
  if (page.interrupted()) {
    return ThrowInterrupted(pCtx);
  }
  page.reportError(text);
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

const JSCFunctionListEntry kPreludeNatives[]{
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
  const NavigatorString strings[]{
      {"userAgent", userAgent}, {"appVersion", appVersion}, {"appName", "Netscape"}, {"appCodeName", "Mozilla"},
      {"product", "Gecko"},     {"platform", ""},           {"vendor", ""},          {"language", language},
  };
  for (const NavigatorString& string : strings) {
    if (!Define(pCtx, navigator.get(), string.name, NewString(pCtx, string.value), JS_PROP_ENUMERABLE)) {
      return JS_EXCEPTION;
    }
  }
  JsValue languages(pCtx, JS_NewArray(pCtx));
  if (JS_IsException(languages.get()) ||
      !Define(pCtx, languages.get(), "0", NewString(pCtx, language), kOrdinaryProperty) ||
      !Define(pCtx, navigator.get(), "languages", languages.release(), JS_PROP_ENUMERABLE) ||
      !Define(pCtx, navigator.get(), "cookieEnabled", JS_TRUE, JS_PROP_ENUMERABLE) ||
      !Define(pCtx, navigator.get(), "onLine", JS_TRUE, JS_PROP_ENUMERABLE) ||
      !Define(pCtx, navigator.get(), "hardwareConcurrency", JS_NewInt32(pCtx, 1), JS_PROP_ENUMERABLE) ||
      !Define(pCtx, navigator.get(), "maxTouchPoints", JS_NewInt32(pCtx, 0), JS_PROP_ENUMERABLE) ||
      !SetFunctions(pCtx, navigator.get(), kNavigatorMethods)) {
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

  // The methods of the built-ins that format with Intl, a lazy part of the prelude.
  for (const BuiltInMethod& method : kIntlMethods) {
    const JsValue constructor(pCtx, JS_GetPropertyStr(pCtx, global.get(), method.constructor));
    const JsValue prototype(pCtx, JS_GetPropertyStr(pCtx, constructor.get(), "prototype"));
    if (JS_IsException(prototype.get()) ||
        !Define(pCtx, prototype.get(), method.name,
                JS_NewCFunction2(pCtx, method.function, method.name, method.length, JS_CFUNC_generic, 0),
                kWritableConfigurable)) {
      return JS_EXCEPTION;
    }
  }

  JsValue natives(pCtx, JS_NewObject(pCtx));
  if (JS_IsException(natives.get()) || !SetFunctions(pCtx, natives.get(), kPreludeNatives)) {
    return JS_EXCEPTION;
  }
  // The object of the lazy globals, that the prelude inserts in the prototype chain of the window.
  const JSClassID lazyGlobalsClass = NewClass(pCtx, "LazyGlobals", &kLazyGlobalsMethods);
  if (lazyGlobalsClass == 0) {
    return JS_EXCEPTION;
  }
  JsValue lazyGlobals(pCtx, JS_NewObjectClass(pCtx, lazyGlobalsClass));
  if (JS_IsException(lazyGlobals.get()) ||
      !Define(pCtx, natives.get(), "lazyGlobals", JS_DupValue(pCtx, lazyGlobals.get()), JS_PROP_ENUMERABLE)) {
    return JS_EXCEPTION;
  }
  page.setLazyGlobals(std::move(lazyGlobals));
  return natives.release();
}

}  // namespace aeronet::internal
