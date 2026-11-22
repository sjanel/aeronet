// The QuickJS-ng engine wrapper of aeronet_web (internal): memory exhaustion of the C++ code of the native functions,
// failed conversions and definitions, class registration, memory exhaustion at the creation of a runtime.
#include <gtest/gtest.h>
#include <quickjs.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <new>
#include <string>

#include "../src/js-engine.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet::internal {
namespace {

constexpr JsLimits kNoLimits{std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max()};

JSValue ThrowsBadAlloc(JSContext* /*pCtx*/, JSValueConst /*self*/) { throw std::bad_alloc(); }

JSValue SetterThrowsBadAlloc(JSContext* /*pCtx*/, JSValueConst /*self*/, JSValueConst /*value*/) {
  throw std::bad_alloc();
}

JSValue MethodThrowsBadAlloc(JSContext* /*pCtx*/, JSValueConst /*self*/, int /*argc*/, JSValueConst* /*argv*/) {
  throw std::bad_alloc();
}

JSValue MagicMethodThrowsBadAlloc(JSContext* /*pCtx*/, JSValueConst /*self*/, int /*argc*/, JSValueConst* /*argv*/,
                                  int /*magic*/) {
  throw std::bad_alloc();
}

// The memory exhaustion of the C++ code of a native function, its only C++ exception, is an out of memory error.
TEST(JsEngineTest, BadAllocBecomesAnOutOfMemoryError) {
  JsEngine engine(kNoLimits);
  JSContext* pCtx = engine.context();
  EXPECT_TRUE(JS_IsException(JsGetter<ThrowsBadAlloc>(pCtx, JS_UNDEFINED)));
  EXPECT_TRUE(engine.takeException().contains("out of memory"));
  EXPECT_TRUE(JS_IsException(JsSetter<SetterThrowsBadAlloc>(pCtx, JS_UNDEFINED, JS_UNDEFINED)));
  EXPECT_TRUE(engine.takeException().contains("out of memory"));
  EXPECT_TRUE(JS_IsException(JsMethod<MethodThrowsBadAlloc>(pCtx, JS_UNDEFINED, 0, nullptr)));
  EXPECT_TRUE(engine.takeException().contains("out of memory"));
  EXPECT_TRUE(JS_IsException(JsMagicMethod<MagicMethodThrowsBadAlloc>(pCtx, JS_UNDEFINED, 0, nullptr, 1)));
  EXPECT_TRUE(engine.takeException().contains("out of memory"));
}

// A string conversion that throws, or of the JS_EXCEPTION of a failed call, fails with the exception pending.
TEST(JsEngineTest, FailedStringConversions) {
  JsEngine engine(kNoLimits);
  engine.enter(SteadyClock::time_point::max());
  JSContext* pCtx = engine.context();
  const JsValue throwing = engine.eval("({ toString() { throw new RangeError('no string') } })", "test");
  const JsString failed(pCtx, throwing.get());
  EXPECT_TRUE(failed.failed());
  EXPECT_TRUE(engine.takeException().starts_with("RangeError: no string"));

  JS_ThrowSyntaxError(pCtx, "pending");
  const JsString ofException(pCtx, JS_EXCEPTION);
  EXPECT_TRUE(ofException.failed());
  EXPECT_TRUE(engine.takeException().starts_with("SyntaxError: pending"));

  const JsString converted(pCtx, JS_NewInt32(pCtx, 42));
  EXPECT_FALSE(converted.failed());
  EXPECT_EQ(converted.view(), "42");
}

// An Error whose conversion throws converts to its message, a string, as JS_ToCStringLen() does (for the stack
// overflows), the exception of the conversion left pending.
TEST(JsEngineTest, ErrorsWhoseConversionThrows) {
  JsEngine engine(kNoLimits);
  engine.enter(SteadyClock::time_point::max());
  JSContext* pCtx = engine.context();
  const JsValue error = engine.eval("Object.assign(new Error('the message'), { toString() { throw 1 } })", "test");
  const JsString message(pCtx, error.get());
  ASSERT_FALSE(message.failed());
  EXPECT_EQ(message.view(), "the message");
  EXPECT_EQ(engine.takeException(), "1");

  const JsValue noMessage = engine.eval("Object.assign(new Error(), { toString() { throw 1 }, message: 2 })", "test");
  EXPECT_TRUE(JsString(pCtx, noMessage.get()).failed());
  EXPECT_EQ(engine.takeException(), "1");
}

int64_t MallocSize(JSRuntime* pRuntime) {
  JSMemoryUsage usage;
  JS_ComputeMemoryUsage(pRuntime, &usage);
  return usage.malloc_size;
}

// A conversion that fails on memory exhaustion keeps no reference to the string it converts: a string, the joined
// string of a rope, a toString() result (JS_ToCStringLen() keeps it, see kToCStringLeaksOnFailure).
TEST(JsEngineTest, FailedConversionsKeepNoString) {
  JsEngine engine(kNoLimits);
  engine.enter(SteadyClock::time_point::max());
  JSContext* pCtx = engine.context();
  JSRuntime* pRuntime = JS_GetRuntime(pCtx);
  // 1 Mi characters each: 2 MiB as UTF-16, 3 MiB as UTF-8.
  static constexpr const char* kRope = R"('€'.repeat(1 << 19) + '€'.repeat(1 << 19))";
  for (const char* source : {R"('€'.repeat(1 << 20))", kRope, R"(({ toString: () => '€'.repeat(1 << 20) }))"}) {
    const int64_t before = MallocSize(pRuntime);
    {
      const JsValue value = engine.eval(source, "test");
      // Room for a joined rope or a toString() result, not for their UTF-8.
      JS_SetMemoryLimit(pRuntime, static_cast<std::size_t>(MallocSize(pRuntime)) + (std::size_t{5} << 19));
      EXPECT_TRUE(JsString(pCtx, value.get()).failed()) << source;
      EXPECT_TRUE(engine.takeException().contains("out of memory"));
      JS_SetMemoryLimit(pRuntime, 0);  // no limit
    }
    EXPECT_LT(MallocSize(pRuntime), before + (int64_t{1} << 20)) << source;
  }

  const JsValue rope = engine.eval(kRope, "test");
  ASSERT_EQ(JS_VALUE_GET_TAG(rope.get()), JS_TAG_STRING_ROPE);
  EXPECT_EQ(JsString(pCtx, rope.get()).view().size(), std::size_t{3} << 20);
}

// Defining the JS_EXCEPTION of a failed creation fails without defining anything.
TEST(JsEngineTest, DefineOfAFailedCreation) {
  JsEngine engine(kNoLimits);
  JSContext* pCtx = engine.context();
  const JsValue object(pCtx, JS_NewObject(pCtx));
  EXPECT_FALSE(Define(pCtx, object.get(), "x", JS_EXCEPTION, JS_PROP_ENUMERABLE));
  const JsValue value(pCtx, JS_GetPropertyStr(pCtx, object.get(), "x"));
  EXPECT_TRUE(JS_IsUndefined(value.get()));
  EXPECT_TRUE(Define(pCtx, object.get(), "y", JS_NewInt32(pCtx, 1), JS_PROP_ENUMERABLE));
}

// A JsArray whose creation or an element failed stays failed: it frees the next values, release() is JS_EXCEPTION.
TEST(JsEngineTest, FailedArrays) {
  JsEngine engine(kNoLimits);
  JSContext* pCtx = engine.context();
  JSRuntime* pRuntime = JS_GetRuntime(pCtx);

  JsArray filled(pCtx);
  filled.append({JS_NewInt32(pCtx, 1), NewString(pCtx, "two")});
  const JsValue array(pCtx, filled.release());
  ASSERT_TRUE(JS_IsArray(array.get()));
  const JsValue second(pCtx, JS_GetPropertyUint32(pCtx, array.get(), 1));
  EXPECT_EQ(JsString(pCtx, second.get()).view(), "two");

  const JSValue kept = NewString(pCtx, "kept");  // created before the limit, freed by the failed array
  JsArray empty(pCtx);
  JS_SetMemoryLimit(pRuntime, 1);                  // no allocation succeeds
  EXPECT_FALSE(empty.push(JS_NewInt32(pCtx, 1)));  // its elements need memory
  EXPECT_FALSE(empty.push(kept));
  JsArray notCreated(pCtx);
  notCreated.append({JS_NewInt32(pCtx, 1)});
  JS_SetMemoryLimit(pRuntime, 0);  // no limit
  EXPECT_TRUE(JS_IsException(empty.release()));
  EXPECT_TRUE(JS_IsException(notCreated.release()));
  static_cast<void>(engine.takeException());
}

TEST(JsEngineTest, ClassesOfARuntimeAreConsecutive) {
  JsEngine engine(kNoLimits);
  const JSClassID first = NewClass(engine.context(), "First");
  EXPECT_EQ(NewClass(engine.context(), "Second"), first + 1);
  JsValue object(engine.context(), JS_NewObjectClass(engine.context(), first));
  EXPECT_EQ(JS_GetClassID(object.get()), first);
}

TEST(JsEngineTest, EvalAndDeadline) {
  JsEngine engine(kNoLimits);
  engine.enter(SteadyClock::time_point::max());
  const JsValue value = engine.eval("6 * 7", "test");
  EXPECT_EQ(JS_VALUE_GET_INT(value.get()), 42);
  EXPECT_FALSE(engine.expired());
  engine.enter(SteadyClock::now());
  EXPECT_TRUE(engine.expired());
  const JsValue interrupted = engine.eval("for (;;) {}", "test");
  EXPECT_TRUE(JS_IsException(interrupted.get()));
  EXPECT_TRUE(engine.interrupted());
  static_cast<void>(engine.takeException());
}

#if AERONET_WANT_MALLOC_OVERRIDES
TEST(JsEngineTest, MemoryExhaustionAtCreation) {
  test::FailNextMalloc(1);
  EXPECT_THROW(JsEngine{kNoLimits}, std::bad_alloc);
  test::ResetToSysMalloc();
}
#endif

}  // namespace
}  // namespace aeronet::internal
