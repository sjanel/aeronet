// The QuickJS-ng engine wrapper of aeronet_web (internal): memory exhaustion of the C++ code of the native functions,
// failed conversions and definitions, class registration, memory exhaustion at the creation of a runtime.
#include <gtest/gtest.h>
#include <quickjs.h>

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
