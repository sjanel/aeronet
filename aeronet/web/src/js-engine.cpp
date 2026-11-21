#include "js-engine.hpp"

#include <quickjs.h>

#include <cassert>
#include <cstddef>
#include <new>
#include <string>
#include <string_view>

#include "aeronet/timedef.hpp"

namespace aeronet::internal {

namespace {

// The built-ins of JS_NewContext() (QuickJS-ng 0.16 and 0.18), and DOMException, thrown by the DOM bindings.
bool AddIntrinsics(JSContext* pCtx) {
  return JS_AddIntrinsicBaseObjects(pCtx) == 0 && JS_AddIntrinsicDate(pCtx) == 0 && JS_AddIntrinsicEval(pCtx) == 0 &&
         JS_AddIntrinsicRegExp(pCtx) == 0 && JS_AddIntrinsicJSON(pCtx) == 0 && JS_AddIntrinsicProxy(pCtx) == 0 &&
         JS_AddIntrinsicMapSet(pCtx) == 0 && JS_AddIntrinsicTypedArrays(pCtx) == 0 &&
         JS_AddIntrinsicPromise(pCtx) == 0 && JS_AddIntrinsicWeakRef(pCtx) == 0 &&
         JS_AddIntrinsicDOMException(pCtx) == 0 && JS_AddIntrinsicAToB(pCtx) == 0 && JS_AddPerformance(pCtx) == 0;
}

// JS_AddIntrinsicTypedArrays() adds SharedArrayBuffer and Atomics, configurable properties of the global object. Their
// names are predefined atoms of QuickJS: nothing is allocated.
void RemoveSharedMemory(JSContext* pCtx) noexcept {
  const JsValue global(pCtx, JS_GetGlobalObject(pCtx));
  for (const char* name : {"SharedArrayBuffer", "Atomics"}) {
    const JSAtom atom = JS_NewAtom(pCtx, name);
    [[maybe_unused]] const int deleted = JS_DeleteProperty(pCtx, global.get(), atom, 0);
    assert(deleted == 1);
    JS_FreeAtom(pCtx, atom);
  }
}

}  // namespace

JSClassID NewClass(JSContext* pCtx, const char* name) noexcept {
  JSRuntime* pRuntime = JS_GetRuntime(pCtx);
  JSClassID classId = 0;
  JS_NewClassID(pRuntime, &classId);
  const JSClassDef def{name, nullptr, nullptr, nullptr, nullptr};
  if (JS_NewClass(pRuntime, classId, &def) < 0) {
    JS_ThrowOutOfMemory(pCtx);
    return 0;
  }
  return classId;
}

const char* NonStringToUtf8(JSContext* pCtx, JSValueConst value, std::size_t& size) noexcept {
  // A string (ropes are joined), owned here: the C string keeps its own reference to it.
  JsValue str(pCtx, JS_ToString(pCtx, value));
  if (JS_IsException(str.get())) {
    // As JS_ToCStringLen(): an Error whose conversion fails (a stack overflow in its toString()) converts to its
    // message, the exception of the conversion left pending.
    if (!JS_IsError(value)) {
      return nullptr;
    }
    str = JsValue(pCtx, JS_GetPropertyStr(pCtx, value, "message"));
    if (JS_VALUE_GET_TAG(str.get()) != JS_TAG_STRING) {
      return nullptr;
    }
  }
  return StringToUtf8(pCtx, str.get(), size);
}

std::string ExceptionText(JSContext* pCtx, JSValueConst exception) {
  // The C strings are freed when an append throws.
  const JsString converted(pCtx, exception);
  if (converted.failed()) {
    // Its conversion threw: the value is unknown.
    JS_FreeValue(pCtx, JS_GetException(pCtx));
    return "uncaught exception";
  }
  std::string text(converted.view());
  if (JS_IsError(exception)) {
    const JsValue stack(pCtx, JS_GetPropertyStr(pCtx, exception, "stack"));
    if (JS_IsString(stack.get())) {
      const JsString stackText(pCtx, stack.get());
      if (!stackText.failed() && !stackText.view().empty()) {
        text.append(1, '\n').append(stackText.view());
      }
    }
  }
  return text;
}

JsEngine::JsEngine(const JsLimits& limits) : _pRuntime(JS_NewRuntime()) {
  if (_pRuntime == nullptr) {
    throw std::bad_alloc();
  }
  JS_SetMemoryLimit(_pRuntime, limits.memory);
  JS_SetMaxStackSize(_pRuntime, limits.stack);
  // A captureless generic lambda converts to the handler of both QuickJS-ng APIs: it takes a JSRuntime* before 0.17,
  // a JSContext* since.
  JS_SetInterruptHandler(
      _pRuntime,
      [](auto*, void* pEngine) -> int {
        JsEngine& engine = *static_cast<JsEngine*>(pEngine);
        if (SteadyClock::now() < engine._deadline) {
          return 0;
        }
        engine._interrupted = true;
        return 1;
      },
      this);
  _pContext = JS_NewContextRaw(_pRuntime);
  if (_pContext == nullptr || !AddIntrinsics(_pContext)) {
    if (_pContext != nullptr) {
      JS_FreeContext(_pContext);
    }
    JS_FreeRuntime(_pRuntime);
    throw std::bad_alloc();
  }
  RemoveSharedMemory(_pContext);
}

JsEngine::~JsEngine() {
  if (_pContext != nullptr) {
    JS_FreeContext(_pContext);
  }
  JS_FreeRuntime(_pRuntime);
}

void JsEngine::enter(SteadyClock::time_point deadline) noexcept {
  _deadline = deadline;
  _interrupted = false;
  JS_UpdateStackTop(_pRuntime);
}

JsValue JsEngine::eval(std::string_view code, const char* filename) const {
  assert(code.data()[code.size()] == '\0');  // NOLINT(readability-simplify-subscript-expr) past the end of the view
  return {_pContext, JS_Eval(_pContext, code.data(), code.size(), filename, JS_EVAL_TYPE_GLOBAL)};
}

std::string JsEngine::takeException() const {
  const JsValue exception(_pContext, JS_GetException(_pContext));
  return ExceptionText(_pContext, exception.get());
}

}  // namespace aeronet::internal
