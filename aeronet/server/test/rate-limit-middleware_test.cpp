#include "aeronet/rate-limit-middleware.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/http-request-view.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/rate-limit.hpp"

// NOTE ON TEST SCOPE
// ------------------
// HttpRequestView has a private constructor and only a short, fixed list of friend classes
// (see http-request-view.hpp), none of which is this test suite. That means a real
// HttpRequestView cannot be constructed here, so the RequestMiddleware returned by build()
// cannot actually be invoked from a unit test at this scope (this test target also only links
// aeronet_http, not aeronet_server/aeronet_test_support). Everything below therefore tests the
// builder's *construction-time* contract: what build() validates, what it defaults, and how the
// two overloads (const& vs &&) treat the builder's members (copy vs move, in particular the
// store). End-to-end behavior of the produced middleware (429 responses, Retry-After, per-strategy
// key resolution against a live request, fail-open/fail-closed under an actual request) belongs in
// an integration-style test that spins up a real server and issues real HTTP requests.

namespace aeronet {
namespace {

// Minimal stand-in store used to prove that build() accepts arbitrary IRateLimitStore
// implementations, not just InMemoryTokenBucketRateLimitStore.
class AlwaysAllowRateLimitStore final : public IRateLimitStore {
 public:
  RateLimitDecision consume(std::string_view /*key*/, std::chrono::steady_clock::time_point /*now*/,
                            const RateLimitConfig& /*config*/) override {
    return RateLimitDecision::Allow();
  }
};

}  // namespace

TEST(RateLimitMiddleware, Nominal) {
  RateLimitRequestMiddlewareBuilder options;

  RequestMiddleware middleware = options.build();

  EXPECT_TRUE(static_cast<bool>(middleware));
}

TEST(RateLimitMiddleware, DefaultBuilderFieldsMatchDocumentedDefaults) {
  RateLimitRequestMiddlewareBuilder options;

  EXPECT_EQ(options.keyStrategy, RateLimitClientKeyStrategy::PeerAddress);
  EXPECT_EQ(options.headerName.get(), "x-forwarded-for");
  EXPECT_EQ(options.rejectionBody, "rate limited");
  EXPECT_EQ(options.store, nullptr);
  EXPECT_FALSE(static_cast<bool>(options.customKeyExtractor));

  EXPECT_EQ(options.config.requestsPerSecond, 10U);
  EXPECT_EQ(options.config.burst, 10U);
  EXPECT_EQ(options.config.maxKeys, 65536U);
  EXPECT_TRUE(options.config.failOpen);
  EXPECT_EQ(options.config.nbShards, 64U);
  EXPECT_EQ(options.config.idleTtl, std::chrono::seconds{300});
}

TEST(RateLimitMiddleware, ConfigValidationErrorsPropagateFromBuild) {
  // build() forwards to RateLimitConfig::validate() before constructing anything, so every
  // invalid-config case covered by RateLimitConfigTest.InvalidValuesThrow (rate-limit_test.cpp)
  // must also surface through the middleware builder.
  RateLimitRequestMiddlewareBuilder options;

  options.config.requestsPerSecond = 0;
  EXPECT_THROW(static_cast<void>(options.build()), std::invalid_argument);

  options.config = {};
  options.config.burst = 1;
  options.config.requestsPerSecond = 2;
  EXPECT_THROW(static_cast<void>(options.build()), std::invalid_argument);

  options.config = {};
  options.config.maxKeys = 0;
  EXPECT_THROW(static_cast<void>(options.build()), std::invalid_argument);

  options.config = {};
  options.config.idleTtl = std::chrono::seconds{0};
  EXPECT_THROW(static_cast<void>(options.build()), std::invalid_argument);

  options.config = {};
  options.config.nbShards = 0;
  EXPECT_THROW(static_cast<void>(options.build()), std::invalid_argument);

  options.config = {};
  EXPECT_NO_THROW(static_cast<void>(options.build()));
}

TEST(RateLimitMiddleware, ConstLvalueBuildLeavesBuilderIntactAndIsRepeatable) {
  RateLimitRequestMiddlewareBuilder options;
  options.config.requestsPerSecond = 5;
  options.config.burst = 5;
  options.rejectionBody = "too fast";

  RequestMiddleware first = options.build();
  RequestMiddleware second = options.build();

  EXPECT_TRUE(static_cast<bool>(first));
  EXPECT_TRUE(static_cast<bool>(second));

  // The const& overload must copy, not consume: every field stays usable afterward.
  EXPECT_EQ(options.config.requestsPerSecond, 5U);
  EXPECT_EQ(options.config.burst, 5U);
  EXPECT_EQ(options.rejectionBody, "too fast");
}

TEST(RateLimitMiddleware, ConstLvalueBuildCopiesInjectedStoreOwnership) {
  auto store = std::make_shared<InMemoryTokenBucketRateLimitStore>(4);
  ASSERT_EQ(store.use_count(), 1);

  RateLimitRequestMiddlewareBuilder options;
  options.store = store;
  EXPECT_EQ(store.use_count(), 2);  // local `store` + options.store

  RequestMiddleware middleware = options.build();

  EXPECT_TRUE(static_cast<bool>(middleware));
  // build() const& copies *this before moving that copy into the closure, so the original
  // builder's store is untouched (still equal to `store`) while the closure holds its own
  // shared ownership: three owners in total.
  EXPECT_EQ(options.store, store);
  EXPECT_EQ(store.use_count(), 3);
}

TEST(RateLimitMiddleware, RvalueBuildMovesBuilderAndDoesNotDuplicateStoreOwnership) {
  auto store = std::make_shared<InMemoryTokenBucketRateLimitStore>(4);

  RateLimitRequestMiddlewareBuilder options;
  options.store = store;
  ASSERT_EQ(store.use_count(), 2);  // local `store` + options.store

  RequestMiddleware middleware = std::move(options).build();

  EXPECT_TRUE(static_cast<bool>(middleware));
  // build() && moves *this directly into the closure: no intermediate copy is made, so the
  // shared_ptr's use_count stays at 2 (local `store` + the closure's copy), and the
  // moved-from builder no longer holds the store.
  EXPECT_EQ(store.use_count(), 2);
  EXPECT_EQ(options.store, nullptr);  // NOLINT(bugprone-use-after-move) -- intentionally inspecting moved-from state
}

TEST(RateLimitMiddleware, AcceptsCustomIRateLimitStoreImplementation) {
  RateLimitRequestMiddlewareBuilder options;
  options.store = std::make_shared<AlwaysAllowRateLimitStore>();

  RequestMiddleware middleware = options.build();

  EXPECT_TRUE(static_cast<bool>(middleware));
}

TEST(RateLimitMiddleware, BuildsSuccessfullyForEveryKeyStrategy) {
  constexpr RateLimitClientKeyStrategy kStrategies[] = {
      RateLimitClientKeyStrategy::PeerAddress,
      RateLimitClientKeyStrategy::XForwardedForFirst,
      RateLimitClientKeyStrategy::HeaderValue,
      RateLimitClientKeyStrategy::Custom,
  };

  for (const auto strategy : kStrategies) {
    RateLimitRequestMiddlewareBuilder options;
    options.keyStrategy = strategy;
    if (strategy == RateLimitClientKeyStrategy::Custom) {
      options.customKeyExtractor = [](const HttpRequestView& /*request*/) -> std::string_view { return "static-key"; };
    }

    RequestMiddleware middleware = options.build();

    EXPECT_TRUE(static_cast<bool>(middleware)) << "keyStrategy index " << static_cast<int>(strategy);
  }
}

TEST(RateLimitMiddleware, CustomStrategyWithoutExtractorBuildsSuccessfully) {
  // No extractor is required at build() time; ResolveKey() only consults customKeyExtractor
  // when a request actually arrives, and simply bypasses rate limiting when it's unset.
  RateLimitRequestMiddlewareBuilder options;
  options.keyStrategy = RateLimitClientKeyStrategy::Custom;

  EXPECT_NO_THROW(static_cast<void>(options.build()));
}

TEST(RateLimitMiddleware, HeaderValueStrategyAcceptsCustomHeaderName) {
  RateLimitRequestMiddlewareBuilder options;
  options.keyStrategy = RateLimitClientKeyStrategy::HeaderValue;
  options.headerName = "x-api-key";

  RequestMiddleware middleware = options.build();

  EXPECT_TRUE(static_cast<bool>(middleware));
  EXPECT_EQ(options.headerName.get(), "x-api-key");
}

TEST(RateLimitMiddleware, FullyCustomizedBuilderBuildsSuccessfully) {
  auto store = std::make_shared<InMemoryTokenBucketRateLimitStore>(8);

  RateLimitRequestMiddlewareBuilder options;
  options.config.requestsPerSecond = 50;
  options.config.burst = 100;
  options.config.maxKeys = 1024;
  options.config.idleTtl = std::chrono::seconds{60};
  options.config.failOpen = false;
  options.config.nbShards = 8;
  options.store = store;
  options.keyStrategy = RateLimitClientKeyStrategy::XForwardedForFirst;
  options.rejectionBody = "please slow down";

  RequestMiddleware middleware = std::move(options).build();

  EXPECT_TRUE(static_cast<bool>(middleware));
}

}  // namespace aeronet
