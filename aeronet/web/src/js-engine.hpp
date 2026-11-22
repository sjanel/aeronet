#pragma once

#include <quickjs.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/timedef.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

// A JSValue owned by this object, freed with its context. The containers of many values keep their context once,
// with plain JSValue (see JsValues, TimerQueue).
class JsValue {
 public:
  using trivially_relocatable = std::true_type;

  JsValue() noexcept = default;

  // Takes the ownership of 'value'.
  JsValue(JSContext* pCtx, JSValue value) noexcept : _pCtx(pCtx), _value(value) {}

  JsValue(const JsValue&) = delete;
  JsValue& operator=(const JsValue&) = delete;

  JsValue(JsValue&& other) noexcept : _pCtx(other._pCtx), _value(std::exchange(other._value, JS_UNDEFINED)) {}

  JsValue& operator=(JsValue&& other) noexcept {
    if (this != &other) {
      reset();
      _pCtx = other._pCtx;
      _value = std::exchange(other._value, JS_UNDEFINED);
    }
    return *this;
  }

  ~JsValue() { reset(); }

  [[nodiscard]] JSValue get() const noexcept { return _value; }

  [[nodiscard]] JSContext* context() const noexcept { return _pCtx; }

  // Gives up the ownership of the value.
  [[nodiscard]] JSValue release() noexcept { return std::exchange(_value, JS_UNDEFINED); }

  void reset() noexcept {
    if (_pCtx != nullptr) {
      JS_FreeValue(_pCtx, std::exchange(_value, JS_UNDEFINED));
    }
  }

 private:
  JSContext* _pCtx{};
  JSValue _value{JS_UNDEFINED};
};

// The native functions report their errors as QuickJS expects: they return JS_EXCEPTION, the exception pending in
// their context, thrown by a JS_Throw*() function or by a QuickJS call that failed. JsMethod, JsGetter, JsSetter and
// JsMagicMethod run the body 'Fn' of a native function of the four QuickJS signatures, turning the only C++ exceptions
// left into an out of memory error: no exception may cross the C frames of QuickJS. They are all memory exhaustion of
// the C++ code: std::bad_alloc of an allocation, std::overflow_error of an amc container grown past its size type.
template <JSValue (*Fn)(JSContext*, JSValueConst, int, JSValueConst*)>
JSValue JsMethod(JSContext* pCtx, JSValueConst thisVal, int argc, JSValueConst* argv) noexcept {
  try {
    return Fn(pCtx, thisVal, argc, argv);
  } catch (...) {
    return JS_ThrowOutOfMemory(pCtx);
  }
}

template <JSValue (*Fn)(JSContext*, JSValueConst)>
JSValue JsGetter(JSContext* pCtx, JSValueConst thisVal) noexcept {
  try {
    return Fn(pCtx, thisVal);
  } catch (...) {
    return JS_ThrowOutOfMemory(pCtx);
  }
}

template <JSValue (*Fn)(JSContext*, JSValueConst, JSValueConst)>
JSValue JsSetter(JSContext* pCtx, JSValueConst thisVal, JSValueConst value) noexcept {
  try {
    return Fn(pCtx, thisVal, value);
  } catch (...) {
    return JS_ThrowOutOfMemory(pCtx);
  }
}

template <JSValue (*Fn)(JSContext*, JSValueConst, int, JSValueConst*, int)>
JSValue JsMagicMethod(JSContext* pCtx, JSValueConst thisVal, int argc, JSValueConst* argv, int magic) noexcept {
  try {
    return Fn(pCtx, thisVal, argc, argv, magic);
  } catch (...) {
    return JS_ThrowOutOfMemory(pCtx);
  }
}

// When the allocation of the UTF-8 fails, JS_ToCStringLen() keeps the reference it took to the string it converts
// (QuickJS-ng 0.16 to 0.18 at least, and its master on 2026-11-21): the string then outlives its runtime. Not assumed
// of the next versions: a fixed one would free the string twice, an unfixed one leaks it again (caught by
// FailedConversionsKeepNoString in web-engine_test.cpp).
inline constexpr bool kToCStringLeaksOnFailure = QJS_VERSION_MAJOR == 0 && QJS_VERSION_MINOR <= 18;

// The UTF-8 of 'str', a string (not a rope), as JS_ToCStringLen() without its leak. Null on memory exhaustion, the
// exception pending. Free it with JS_FreeCString().
inline const char* StringToUtf8(JSContext* pCtx, JSValueConst str, std::size_t& size) noexcept {
  assert(JS_VALUE_GET_TAG(str) == JS_TAG_STRING);
  const char* pUtf8 = JS_ToCStringLen(pCtx, &size, str);
  if constexpr (kToCStringLeaksOnFailure) {
    if (pUtf8 == nullptr) [[unlikely]] {
      // The only failure for a string, after its reference was taken.
      JS_FreeValue(pCtx, str);
    }
  }
  return pUtf8;
}

// StringToUtf8() of String(value), for a value other than a string: JS_ToCStringLen() would leak the string of its
// own conversion. Null when the conversion throws, the exception pending.
const char* NonStringToUtf8(JSContext* pCtx, JSValueConst value, std::size_t& size) noexcept;

// The UTF-8 of String(value), as JS_ToCStringLen() without its leak. Null when the conversion throws, the exception
// pending. Free it with JS_FreeCString().
inline const char* ToUtf8(JSContext* pCtx, JSValueConst value, std::size_t& size) noexcept {
  if (JS_VALUE_GET_TAG(value) == JS_TAG_STRING) [[likely]] {
    return StringToUtf8(pCtx, value, size);
  }
  return NonStringToUtf8(pCtx, value, size);
}

// The string conversion of 'value' (JavaScript String(value)), which fails when it throws: check failed() first, the
// exception is pending. The conversion of JS_EXCEPTION, the result of a failed call, fails too.
// A local object: inline, and with its size read through a local variable, it does not escape, so that the compiler
// keeps its fields in registers (no context pointer stored, the one of the caller is reused).
class JsString {
 public:
  JsString(JSContext* pCtx, JSValueConst value) noexcept : _pCtx(pCtx) { convert(value); }

  JsString(const JsString&) = delete;
  JsString& operator=(const JsString&) = delete;
  JsString(JsString&&) = delete;
  JsString& operator=(JsString&&) = delete;

  ~JsString() { JS_FreeCString(_pCtx, _pStr); }  // nothing for a null pointer

  // Whether the conversion failed, its exception pending in the context.
  [[nodiscard]] bool failed() const noexcept { return _size == kFailed; }

  // UTF-8, null-terminated at size() (unless empty for a NullIsEmpty null). Not for a failed conversion.
  [[nodiscard]] std::string_view view() const noexcept {
    assert(!failed());
    return {_pStr, _size};
  }

 protected:
  struct NullIsEmpty {};

  // null converts to the empty string, not "null" (the nullable DOMString setters: textContent, innerHTML...).
  JsString(JSContext* pCtx, JSValueConst value, NullIsEmpty /*tag*/) noexcept : _pCtx(pCtx) {
    if (JS_IsNull(value)) {
      _pStr = nullptr;
      _size = 0;
    } else {
      convert(value);
    }
  }

 private:
  static constexpr std::size_t kFailed = std::numeric_limits<std::size_t>::max();

  void convert(JSValueConst value) noexcept {
    std::size_t size = 0;
    _pStr = JS_IsException(value) ? nullptr : ToUtf8(_pCtx, value, size);
    _size = _pStr == nullptr ? kFailed : size;
  }

  JSContext* _pCtx;
  const char* _pStr;
  std::size_t _size;
};

// The string conversion of argv[index], of "undefined" when absent, as the DOM converts a missing DOMString argument.
class JsArgument : public JsString {
 public:
  JsArgument(JSContext* pCtx, int argc, JSValueConst* argv, int index) noexcept
      : JsString(pCtx, index < argc ? argv[index] : JS_UNDEFINED) {}
};

// Property flags. The JS_PROP_* flags are distinct bits of signed int macros: added rather than or-ed
// (bugprone-signed-bitwise).
inline constexpr int kWritableConfigurable = JS_PROP_WRITABLE + JS_PROP_CONFIGURABLE;
inline constexpr int kEnumerableWritable = JS_PROP_ENUMERABLE + JS_PROP_WRITABLE;
// The flags of a property created by an assignment, and of the elements of an array.
inline constexpr int kOrdinaryProperty = kWritableConfigurable + JS_PROP_ENUMERABLE;

// 'N' values JS_UNDEFINED.
template <std::size_t N>
std::array<JSValue, N> UndefinedValues() noexcept {
  std::array<JSValue, N> values;
  values.fill(JS_UNDEFINED);
  return values;
}

// Throws the uncatchable error of the deadline, as QuickJS-ng does when its interrupt handler stops a script. Returns
// JS_EXCEPTION.
inline JSValue ThrowInterrupted(JSContext* pCtx) noexcept {
  JS_ThrowInternalError(pCtx, "interrupted");
  const JSValue error = JS_GetException(pCtx);
  JS_SetUncatchableError(pCtx, error);
  return JS_Throw(pCtx, error);
}

// A JavaScript string of 'str' (UTF-8), JS_EXCEPTION on memory exhaustion.
inline JSValue NewString(JSContext* pCtx, std::string_view str) noexcept {
  return JS_NewStringLen(pCtx, str.data(), str.size());
}

// A JavaScript array, filled with owned values. Defining its elements runs no script (no setter of Array.prototype),
// so that the tree cannot change while it is filled. A failure (of its creation, or of an element) makes it failed for
// good, the exception pending.
class JsArray {
 public:
  explicit JsArray(JSContext* pCtx) noexcept : _array(pCtx, JS_NewArray(pCtx)) {}

  [[nodiscard]] JSContext* context() const noexcept { return _array.context(); }

  // Appends 'value' (owned, freed on failure too), which can be the JS_EXCEPTION of a failed creation. Returns false
  // when the array failed.
  [[nodiscard]] bool push(JSValue value) noexcept {
    JSContext* pCtx = _array.context();
    if (JS_IsException(_array.get())) {
      JS_FreeValue(pCtx, value);
      return false;
    }
    // JS_DefinePropertyValueUint32 frees the value on its failure too (an exception has nothing to free).
    if (JS_IsException(value) ||
        JS_DefinePropertyValueUint32(pCtx, _array.get(), _size++, value, kOrdinaryProperty) < 0) {
      _array = JsValue(pCtx, JS_EXCEPTION);
      return false;
    }
    return true;
  }

  // Appends 'values' in turn, as push() of each: after a failure, the next ones are freed. release() tells whether the
  // array failed.
  void append(std::initializer_list<JSValue> values) noexcept {
    for (const JSValue value : values) {
      static_cast<void>(push(value));
    }
  }

  // The array, JS_EXCEPTION if it failed.
  JSValue release() noexcept { return _array.release(); }

 private:
  JsValue _array;
  uint32_t _size{};
};

// Registers in the runtime of pCtx the class 'classId' (allocated by JS_NewClassID()) of objects named 'name', without
// finalizer: their opaque pointers are not owned. 'pExotic' (can be null) are its exotic behaviors. Returns false, with
// an out of memory error thrown, on memory exhaustion.
[[nodiscard]] bool RegisterJsClass(JSContext* pCtx, JSClassID classId, const char* name,
                                   const JSClassExoticMethods* pExotic = nullptr) noexcept;

// Allocates and registers a class (see RegisterJsClass()). Returns its id, 0 (JS_INVALID_CLASS_ID) with an out of
// memory error thrown on memory exhaustion.
[[nodiscard]] JSClassID NewClass(JSContext* pCtx, const char* name,
                                 const JSClassExoticMethods* pExotic = nullptr) noexcept;

// Defines 'functions' (getters, setters, methods) on 'object'. Returns false on memory exhaustion.
[[nodiscard]] inline bool SetFunctions(JSContext* pCtx, JSValueConst object,
                                       std::span<const JSCFunctionListEntry> functions) noexcept {
  return JS_SetPropertyFunctionList(pCtx, object, functions.data(), static_cast<int>(functions.size())) == 0;
}

// Defines object[name] = value (owned, freed on failure too) with 'flags'. 'value' can be the JS_EXCEPTION of a failed
// creation. Returns false on failure, the exception pending.
[[nodiscard]] inline bool Define(JSContext* pCtx, JSValueConst object, const char* name, JSValue value,
                                 int flags) noexcept {
  return !JS_IsException(value) && JS_DefinePropertyValueStr(pCtx, object, name, value, flags) >= 0;
}

// Sets the opaque pointer of 'object', of a class registered by NewClass() (which cannot fail).
inline void SetOpaque(JSValueConst object, void* pOpaque) noexcept {
  [[maybe_unused]] const int ret = JS_SetOpaque(object, pOpaque);
  assert(ret == 0);
}

// Throws the TypeError of a binding called on an object of another class. Returns JS_EXCEPTION.
inline JSValue ThrowIllegalInvocation(JSContext* pCtx) noexcept {
  return JS_ThrowTypeError(pCtx, "Illegal invocation");
}

// Frees 'values', owned by a container that keeps their context once.
inline void FreeValues(JSContext* pCtx, std::span<const JSValue> values) noexcept {
  for (const JSValue value : values) {
    JS_FreeValue(pCtx, value);
  }
}

// The text of a thrown value: "Error: message" followed by the stack for an Error, its string conversion otherwise.
std::string ExceptionText(JSContext* pCtx, JSValueConst exception);

// The deleter of the memory allocated by QuickJS for a context (JS_WriteObject()).
struct JsFreeDeleter {
  void operator()(uint8_t* pData) const noexcept { js_free(pCtx, pData); }

  JSContext* pCtx;
};

// Values owned by this object, freed with their context on destruction.
class JsValues {
 public:
  explicit JsValues(JSContext* pCtx) noexcept : _pCtx(pCtx) {}

  JsValues(const JsValues&) = delete;
  JsValues& operator=(const JsValues&) = delete;
  JsValues(JsValues&&) = delete;
  JsValues& operator=(JsValues&&) = delete;

  ~JsValues() { FreeValues(_pCtx, _values); }

  // Keeps a new reference on 'value'.
  void keep(JSValueConst value) {
    // The slot first, in its own statement (the right operand of an assignment is evaluated before the left one): it
    // is the only step that can throw, the new reference cannot leak.
    JSValue& slot = _values.emplace_back(JS_UNDEFINED);
    slot = JS_DupValue(_pCtx, value);
  }

  [[nodiscard]] JSValueConst front() const noexcept { return _values.front(); }

 private:
  vector<JSValue> _values;
  JSContext* _pCtx;
};

// The memory and stack limits of a runtime, std::numeric_limits<std::size_t>::max() for none.
struct JsLimits {
  std::size_t memory;
  std::size_t stack;
};

// A QuickJS runtime and its context, whose global object has the standard built-ins (ES2023+, eval, JSON, Proxy,
// typed arrays, Promise, WeakRef, DOMException, atob / btoa, performance), without SharedArrayBuffer and Atomics: no
// script needs them, and they were the cause of several memory-safety bugs of the engine. No file, network or process
// access: quickjs-libc is not used. The scripts stop at the deadline (see setDeadline()).
class JsEngine {
 public:
  // Throws std::bad_alloc on memory exhaustion.
  explicit JsEngine(const JsLimits& limits);

  JsEngine(const JsEngine&) = delete;
  JsEngine& operator=(const JsEngine&) = delete;
  JsEngine(JsEngine&&) = delete;
  JsEngine& operator=(JsEngine&&) = delete;

  ~JsEngine();

  [[nodiscard]] JSRuntime* runtime() const noexcept { return _pRuntime; }
  [[nodiscard]] JSContext* context() const noexcept { return _pContext; }

  // Starts a period of script execution from C++, which stops the scripts at 'deadline'. The stack limit is computed
  // from the current stack: call it from the frame that runs the scripts, not from a native function.
  void enter(SteadyClock::time_point deadline) noexcept;

  // Whether the deadline stopped a script since enter(): the scripts run after it are stopped at once.
  [[nodiscard]] bool interrupted() const noexcept { return _interrupted; }

  // Whether the deadline has passed.
  [[nodiscard]] bool expired() const noexcept { return _interrupted || SteadyClock::now() >= _deadline; }

  // Runs 'code' (UTF-8, null-terminated at code.size()) as a classic script named 'filename' (in the stack traces),
  // and returns its completion value, or JS_EXCEPTION with the exception pending.
  [[nodiscard]] JsValue eval(std::string_view code, const char* filename) const;

  // Takes the pending exception of the context (see ExceptionText()).
  [[nodiscard]] std::string takeException() const;

  // Runs the pending jobs (promise reactions, queueMicrotask() callbacks) until none is left or the deadline passes,
  // calling onError(std::string) with the text of each exception they throw.
  template <class OnError>
  void runMicrotasks(OnError onError) {
    JSContext* pJobCtx = nullptr;
    for (int ret = 0; !expired() && (ret = JS_ExecutePendingJob(_pRuntime, &pJobCtx)) != 0;) {
      if (ret < 0 && !_interrupted) {
        onError(takeException());
      }
    }
  }

 private:
  JSRuntime* _pRuntime;
  JSContext* _pContext{};
  SteadyClock::time_point _deadline{SteadyClock::time_point::max()};
  bool _interrupted{false};
};

}  // namespace aeronet::internal
