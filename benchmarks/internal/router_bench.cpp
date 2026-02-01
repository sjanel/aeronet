// Router benchmarks measuring route matching performance under
// various configurations:
//  - Literal-only paths (static routes)
//  - Complex patterned routes with parameters and wildcards
//  - Routes with similar prefixes (worst case prefix splitting)
//  - Routes with different prefixes (best case quick routing)

#include <benchmark/benchmark.h>

#include <random>
#include <string>
#include <string_view>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/router.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

namespace {

using http::Method;

std::mt19937_64 gen;

// Trivial handler returning a fixed response
HttpResponse OkHandler([[maybe_unused]] const HttpRequest& req) { return HttpResponse("OK"); }

struct MethodAndPath {
  Method method;
  std::string_view path;
};

struct RouterWithRoutes {
  void set(Method method, std::string_view path) {
    paths.emplace_back(method, path);
    router.setPath(method, path, OkHandler);
  }

  void setMissing(Method method, std::string_view path) {
    paths.emplace_back(method, path);
    // Do not actually add to router to simulate missing route
  }

  const MethodAndPath& pickRandomPath() {
    std::uniform_int_distribution<uint32_t> dist(0, paths.size() - 1);
    return paths[dist(gen)];
  }

  auto match(Method method, std::string_view path) { return router.match(method, path); }

  auto allowedMethods(std::string_view path) { return router.allowedMethods(path); }

  vector<MethodAndPath> paths;
  Router router;
};

}  // namespace

// -----------------------------------------------------------------------------
// Fixture: Literal-only routes (e.g., /api/v1/users, /health, /metrics)
// Common API server pattern with static paths only
// -----------------------------------------------------------------------------
class LiteralRoutesFixture : public benchmark::Fixture {
 public:
  void SetUp(const benchmark::State& /* state */) override {
    // Simulate typical API server routes
    router.set(Method::GET, "/");
    router.set(Method::GET, "/health");
    router.set(Method::GET, "/metrics");
    router.set(Method::GET, "/api/v1/users");
    router.set(Method::POST, "/api/v1/users");
    router.set(Method::GET, "/api/v1/orders");
    router.set(Method::POST, "/api/v1/orders");
    router.set(Method::GET, "/api/v1/products");
    router.set(Method::GET, "/api/v1/categories");
    router.set(Method::GET, "/api/v2/users");
    router.set(Method::POST, "/api/v2/users");
    router.set(Method::GET, "/api/v2/orders");
    router.set(Method::GET, "/admin/dashboard");
    router.set(Method::GET, "/admin/settings");
    router.set(Method::POST, "/admin/settings");

    // Some missing routes used for random matching
    router.setMissing(Method::DELETE, "/api/v1/users");
    router.setMissing(Method::PUT, "/api/v1/orders");
    router.setMissing(Method::GET, "/api/v20/users");
  }

  void TearDown(const benchmark::State& /* state */) override { router = {}; }

  RouterWithRoutes router;
};

BENCHMARK_F(LiteralRoutesFixture, MatchRoot)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(LiteralRoutesFixture, MatchShortPath)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/health");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(LiteralRoutesFixture, MatchMediumPath)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/api/v1/users");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(LiteralRoutesFixture, MatchDeepPath)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/admin/dashboard");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(LiteralRoutesFixture, MatchNonExistent)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/does/not/exist");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(LiteralRoutesFixture, MatchRandomPaths)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    const auto& mp = router.pickRandomPath();
    auto result = router.match(mp.method, mp.path);
    benchmark::DoNotOptimize(result);
  }
}

// -----------------------------------------------------------------------------
// Fixture: Patterned routes with parameters and wildcards
// REST API with dynamic resource IDs
// -----------------------------------------------------------------------------
class PatternedRoutesFixture : public benchmark::Fixture {
 public:
  void SetUp(const benchmark::State& /* state */) override {
    // REST resources with path parameters
    router.set(Method::GET, "/users/{id}");
    router.set(Method::PUT, "/users/{id}");
    router.set(Method::DELETE, "/users/{id}");
    router.set(Method::GET, "/users/{id}/posts");
    router.set(Method::GET, "/users/{id}/posts/{postId}");
    router.set(Method::PUT, "/users/{id}/posts/{postId}");
    router.set(Method::GET, "/users/{id}/posts/{postId}/comments");
    router.set(Method::GET, "/users/{id}/posts/{postId}/comments/{commentId}");

    // Wildcard routes
    router.set(Method::GET, "/static/*");
    router.set(Method::GET, "/files/*");
    router.set(Method::GET, "/assets/images/*");

    // Mixed patterns
    router.set(Method::GET, "/api/v{version}/resource");
    router.set(Method::GET, "/item-{id}-detail");

    // Some missing paths for random matching
    router.setMissing(Method::GET, "/users/{id}/profile");
    router.setMissing(Method::POST, "/users/{id}/posts/{postId}/comments");
    router.setMissing(Method::GET, "/static/images/*");
  }

  void TearDown(const benchmark::State& /* state */) override { router = {}; }

  RouterWithRoutes router;
};

BENCHMARK_F(PatternedRoutesFixture, MatchSingleParam)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/users/12345");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(PatternedRoutesFixture, MatchMultipleParams)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/users/123/posts/456/comments/789");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(PatternedRoutesFixture, MatchWildcard)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/static/css/main.css");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(PatternedRoutesFixture, MatchDeepWildcard)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/files/a/b/c/d/e/f/g.txt");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(PatternedRoutesFixture, MatchMixedPattern)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/api/v2/resource");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(PatternedRoutesFixture, MatchInlineParam)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/item-42-detail");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(PatternedRoutesFixture, MatchRandomPaths)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    const auto& mp = router.pickRandomPath();
    auto result = router.match(mp.method, mp.path);
    benchmark::DoNotOptimize(result);
  }
}

// -----------------------------------------------------------------------------
// Fixture: Routes with similar prefixes (stress prefix splitting)
// Worst-case scenario for some tree: many routes that share long common prefixes
// -----------------------------------------------------------------------------
class SimilarPrefixesFixture : public benchmark::Fixture {
 public:
  void SetUp(const benchmark::State& /* state */) override {
    // All routes share /api/users prefix, forcing multiple prefix splits
    router.set(Method::GET, "/api/users");
    router.set(Method::GET, "/api/user");
    router.set(Method::GET, "/api/user-settings");
    router.set(Method::GET, "/api/user-profile");
    router.set(Method::GET, "/api/user-preferences");
    router.set(Method::GET, "/api/users-list");
    router.set(Method::GET, "/api/users-active");
    router.set(Method::GET, "/api/users-inactive");
    router.set(Method::GET, "/api/users/{id}");
    router.set(Method::GET, "/api/users/{id}/profile");
    router.set(Method::GET, "/api/users/{id}/settings");
    router.set(Method::GET, "/api/users/{id}/preferences");
    router.set(Method::GET, "/api/users/{id}/profile/avatar");
    router.set(Method::GET, "/api/users/{id}/profile/cover");

    // Another cluster sharing /api/orders prefix
    router.set(Method::GET, "/api/orders");
    router.set(Method::GET, "/api/order");
    router.set(Method::GET, "/api/order-items");
    router.set(Method::GET, "/api/order-status");
    router.set(Method::GET, "/api/orders-pending");
    router.set(Method::GET, "/api/orders-completed");

    // Some missing paths for random matching
    router.setMissing(Method::GET, "/api/user-friends");
    router.setMissing(Method::GET, "/api/users/{id}/notifications");
    router.setMissing(Method::GET, "/api/orders-history");
  }

  void TearDown(const benchmark::State& /* state */) override { router = {}; }

  RouterWithRoutes router;
};

BENCHMARK_F(SimilarPrefixesFixture, MatchExactPrefix)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/api/users");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(SimilarPrefixesFixture, MatchShortPrefix)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/api/user");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(SimilarPrefixesFixture, MatchSuffixVariant)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/api/user-preferences");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(SimilarPrefixesFixture, MatchPluralVariant)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/api/users-active");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(SimilarPrefixesFixture, MatchParameterized)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/api/users/42/profile/avatar");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(SimilarPrefixesFixture, MatchDifferentCluster)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/api/orders-pending");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(SimilarPrefixesFixture, MatchRandomPaths)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    const auto& mp = router.pickRandomPath();
    auto result = router.match(mp.method, mp.path);
    benchmark::DoNotOptimize(result);
  }
}

// -----------------------------------------------------------------------------
// Fixture: Routes with completely different prefixes (best case)
// Routes branch early, minimal prefix comparison per lookup
// -----------------------------------------------------------------------------
class DifferentPrefixesFixture : public benchmark::Fixture {
 public:
  void SetUp(const benchmark::State& /* state */) override {
    // Completely different first characters/segments
    router.set(Method::GET, "/alpha/resource");
    router.set(Method::GET, "/beta/resource");
    router.set(Method::GET, "/gamma/resource");
    router.set(Method::GET, "/delta/resource");
    router.set(Method::GET, "/epsilon/resource");
    router.set(Method::GET, "/zeta/resource");
    router.set(Method::GET, "/eta/resource");
    router.set(Method::GET, "/theta/resource");
    router.set(Method::GET, "/iota/resource");
    router.set(Method::GET, "/kappa/resource");
    router.set(Method::GET, "/1/data");
    router.set(Method::GET, "/2/data");
    router.set(Method::GET, "/3/data");
    router.set(Method::GET, "/9/data");
    router.set(Method::GET, "/_internal/debug");
    router.set(Method::GET, "/-special/path");

    // Some missing paths for random matching
    router.setMissing(Method::GET, "/unknown/resource");
    router.setMissing(Method::GET, "/missing/data");
    router.setMissing(Method::GET, "/void/path");
  }

  void TearDown(const benchmark::State& /* state */) override { router = {}; }

  RouterWithRoutes router;
};

BENCHMARK_F(DifferentPrefixesFixture, MatchFirstInList)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/alpha/resource");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(DifferentPrefixesFixture, MatchMiddle)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/epsilon/resource");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(DifferentPrefixesFixture, MatchLast)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/-special/path");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(DifferentPrefixesFixture, MatchNumeric)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/9/data");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(DifferentPrefixesFixture, MatchNonExistent)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/unknown/path");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(DifferentPrefixesFixture, MatchRandomPaths)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    const auto& mp = router.pickRandomPath();
    auto result = router.match(mp.method, mp.path);
    benchmark::DoNotOptimize(result);
  }
}

// -----------------------------------------------------------------------------
// Fixture: Large route table (scalability test)
// Simulates a large microservice with many endpoints
// -----------------------------------------------------------------------------
class LargeRouteTableFixture : public benchmark::Fixture {
 public:
  void SetUp(const benchmark::State& /* state */) override {
    // Generate 200+ routes to stress the tree
    static constexpr std::string_view kResources[] = {"users",   "posts",  "comments", "likes",         "shares",
                                                      "follows", "blocks", "messages", "notifications", "settings"};

    static constexpr std::string_view kVersions[] = {"v1", "v2", "v3"};

    for (auto version : kVersions) {
      for (auto resource : kResources) {
        std::string basePath = std::string("/api/") + std::string(version) + "/" + std::string(resource);

        router.set(Method::GET, basePath);
        router.set(Method::POST, basePath);
        router.set(Method::GET, basePath + "/{id}");
        router.set(Method::PUT, basePath + "/{id}");
        router.set(Method::DELETE, basePath + "/{id}");
        router.set(Method::GET, basePath + "/{id}/details");
        router.set(Method::GET, basePath + "/{id}/history");

        // some missing paths for random matching
        router.setMissing(Method::PATCH, basePath + "/{id}");
        router.setMissing(Method::GET, basePath + "/{id}/stats");
      }
    }

    // Add some static endpoints
    router.set(Method::GET, "/health");
    router.set(Method::GET, "/ready");
    router.set(Method::GET, "/metrics");
    router.set(Method::GET, "/static/*");

    // Some missing paths for random matching
    router.setMissing(Method::GET, "/api/v4/users");
    router.setMissing(Method::GET, "/api/v2/unknown/resource");
    router.setMissing(Method::POST, "/api/v1/posts/{id}/comments");
  }

  void TearDown(const benchmark::State& /* state */) override { router = {}; }

  RouterWithRoutes router;
};

BENCHMARK_F(LargeRouteTableFixture, MatchEarlyVersionEarlyResource)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/api/v1/users");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(LargeRouteTableFixture, MatchLateVersionLateResource)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/api/v3/settings/123/history");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(LargeRouteTableFixture, MatchStaticEndpoint)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/health");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(LargeRouteTableFixture, MatchWildcard)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/static/images/logo.png");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(LargeRouteTableFixture, MatchNonExistentDeep)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/api/v4/unknown/resource/path");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(LargeRouteTableFixture, MatchRandomPaths)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    const auto& mp = router.pickRandomPath();
    auto result = router.match(mp.method, mp.path);
    benchmark::DoNotOptimize(result);
  }
}

// -----------------------------------------------------------------------------
// Fixture: Method lookup performance
// Same path, different methods
// -----------------------------------------------------------------------------
class MethodLookupFixture : public benchmark::Fixture {
 public:
  void SetUp(const benchmark::State& /* state */) override {
    // Register all common methods for one path
    router.set(Method::GET, "/resource");
    router.set(Method::POST, "/resource");
    router.set(Method::PUT, "/resource");
    router.set(Method::DELETE, "/resource");
    router.set(Method::PATCH, "/resource");

    // OPTIONS is not registered to test missing method lookup
    router.setMissing(Method::OPTIONS, "/resource");
  }

  void TearDown(const benchmark::State& /* state */) override { router = {}; }

  RouterWithRoutes router;
};

BENCHMARK_F(MethodLookupFixture, MatchGET)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::GET, "/resource");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(MethodLookupFixture, MatchPOST)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::POST, "/resource");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(MethodLookupFixture, MatchDELETE)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::DELETE, "/resource");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(MethodLookupFixture, MatchOPTIONS_NotRegistered)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.match(Method::OPTIONS, "/resource");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(MethodLookupFixture, AllowedMethods)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    auto result = router.allowedMethods("/resource");
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK_F(MethodLookupFixture, MatchRandomPaths)(benchmark::State& st) {
  for ([[maybe_unused]] auto iter : st) {
    const auto& mp = router.pickRandomPath();
    auto result = router.match(mp.method, mp.path);
    benchmark::DoNotOptimize(result);
  }
}

}  // namespace aeronet

BENCHMARK_MAIN();
