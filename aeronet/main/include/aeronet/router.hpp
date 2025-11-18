#pragma once

#include <amc/type_traits.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

class HttpServer;

class Router {
 public:
  // Classic request handler type: receives a const HttpRequest& and returns an HttpResponse.
  using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

  // Streaming request handler type: receives a const HttpRequest& and an HttpResponseWriter&
  // Use it for large or long-lived responses where sending partial data before completion is beneficial.
  using StreamingHandler = std::function<void(const HttpRequest&, HttpResponseWriter&)>;

  using RequestMiddlewareRange = std::span<const RequestMiddleware>;

  using ResponseMiddlewareRange = std::span<const ResponseMiddleware>;

  // Object that stores handlers and options for a specific group of paths.
  class PathHandlerEntry {
   public:
    // Attach given corsPolicy to the path handler entry.
    PathHandlerEntry& cors(CorsPolicy corsPolicy);

    // Register middleware executed before the route handler. The middleware may mutate
    // the request and short-circuit the chain by returning a response.
    PathHandlerEntry& before(RequestMiddleware middleware);

    // Register middleware executed after the route handler produces a response. The middleware
    // can amend headers or body before the response is finalized.
    PathHandlerEntry& after(ResponseMiddleware middleware);

   private:
    friend class Router;
    friend class HttpServer;

    PathHandlerEntry() noexcept = default;

    http::MethodBmp normalMethodBmp{};
    http::MethodBmp streamingMethodBmp{};
    std::array<RequestHandler, http::kNbMethods> normalHandlers{};
    std::array<StreamingHandler, http::kNbMethods> streamingHandlers{};
    // Optional per-route CorsPolicy stored by value. If set, match() will return a pointer to it.
    CorsPolicy corsPolicy;
    vector<RequestMiddleware> preMiddleware;
    vector<ResponseMiddleware> postMiddleware;
  };

  // Creates an empty Router with a 'Normalize' trailing policy.
  //
  // This default constructor intentionally creates a router with a sane default configuration
  // that normalizes trailing slashes. Use the explicit Router(RouterConfig) constructor
  // to change the trailing slash policy and other router-level options.
  Router() noexcept = default;

  // Creates an empty Router with the configuration taken from the provided object.
  //
  // The RouterConfig controls routing behavior such as trailing slash handling and
  // other matching policies. Constructing a Router with a custom RouterConfig allows
  // the caller to opt into strict trailing slash semantics or automatic normalization.
  explicit Router(RouterConfig config);

  // Register a request middleware executed before any matched handler (including defaults).
  void addRequestMiddleware(RequestMiddleware middleware);

  // Register a response middleware executed after handlers (or short-circuited pre hooks).
  void addResponseMiddleware(ResponseMiddleware middleware);

  // Access the global pre middleware chain.
  // The items are ordered from first to last executed.
  [[nodiscard]] RequestMiddlewareRange globalRequestMiddleware() const noexcept { return _globalPreMiddleware; }

  // Access the global post middleware chain.
  // The items are ordered from first to last executed.
  [[nodiscard]] ResponseMiddlewareRange globalResponseMiddleware() const noexcept { return _globalPostMiddleware; }

  // Copy operations duplicate the router state including all registered handlers.
  Router(const Router& other);
  Router& operator=(const Router& other);

  // Move operations transfer ownership of the router state.
  // The Router being moved from should not be used except for destruction or assignment.
  Router(Router&&) noexcept = default;
  Router& operator=(Router&&) noexcept = default;

  ~Router() = default;

  // Register a global (fallback) request handler invoked when no path-specific handler
  // matches. The handler receives a const HttpRequest& and returns an HttpResponse by value.
  //
  // Behavior and precedence:
  //   - Per-path handlers win over global handlers. If a path has a streaming or normal
  //     handler registered for the request method, that handler will be invoked instead.
  //   - A global streaming handler can be installed separately via setDefault(StreamingHandler).
  //
  // Threading / lifetime:
  //   - Router and its handlers are expected to be used from the single-threaded event loop.
  //     Installing or replacing handlers from other threads is unsupported.
  //
  // Performance:
  //   - Keep handlers lightweight; long-running operations should be dispatched to worker
  //     threads to avoid blocking the event loop.
  void setDefault(RequestHandler handler);

  // Register a global streaming handler that can produce responses incrementally via
  // HttpResponseWriter. Use streaming handlers for large or long-lived responses where
  // sending partial data before completion is beneficial.
  //
  // Lifetime and threading notes are identical to setDefault(RequestHandler).
  void setDefault(StreamingHandler handler);

  // Register a handler for a specific absolute path and a set of allowed HTTP methods.
  //
  // Path can have pattern elements (e.g. /items/{id}/details).
  // Pattern names are optional, and will be given 0-indexed names if omitted.
  // However, it's not possible to have both named and unnamed patterns in the same path.
  // If you want literal { or } match without patterns, use {{ and }} to escape them.
  // Examples:
  // - "/users/{userId}/posts/{post}" matches paths like "/users/42/posts/foo" with userId=42 and post=foo
  // - "/files/{{config}}/data" matches the literal path "/files/{config}/data"
  // - "/items/{}/details-{}" matches paths like "/items/123/details-foo" with "0"=123, "1"=foo
  //
  // You can then retrieve matched pattern values from HttpRequest::pathParams().
  // Path patterns support literal fragments and parameter fragments inside the same
  // segment (for example: `/api/v{}/foo{}bar`).
  //
  // A terminal wildcard `*` is supported (for example: `/files/*`) but must be the
  // final segment of the pattern and does not produce path-parameter captures.
  // Returns the PathHandlerEntry allowing further configuration (e.g. per-route CORS policy).
  // The returned reference is valid until the next call to setPath.
  PathHandlerEntry& setPath(http::MethodBmp methods, std::string_view path, RequestHandler handler);

  // Register a handler for a specific absolute path and a unique allowed HTTP method.
  // See the multi-method overload for details on pattern syntax and capture semantics.
  // Returns the PathHandlerEntry allowing further configuration (e.g. per-route CORS policy).
  // The returned reference is valid until the next call to setPath.
  PathHandlerEntry& setPath(http::Method method, std::string_view path, RequestHandler handler);

  // Register a streaming handler for the provided path and methods. See setPath overloads
  // for general behavior notes. Streaming handlers receive an HttpResponseWriter and may
  // emit response bytes incrementally.
  // Returns the PathHandlerEntry allowing further configuration (e.g. per-route CORS policy).
  // The returned reference is valid until the next call to setPath.
  PathHandlerEntry& setPath(http::MethodBmp methods, std::string_view path, StreamingHandler handler);

  // Register a streaming handler for the provided path and single method. See the multi-method
  // overload for details on pattern syntax, parameter limits, and wildcard rules.
  // Returns the PathHandlerEntry allowing further configuration (e.g. per-route CORS policy).
  // The returned reference is valid until the next call to setPath.
  PathHandlerEntry& setPath(http::Method method, std::string_view path, StreamingHandler handler);

  struct PathParamCapture {
    std::string_view key;
    std::string_view value;
  };

  struct RoutingResult {
    enum class RedirectSlashMode : int8_t {
      None,        // Indicates that no redirection is needed
      AddSlash,    // Indicates that a redirection to add a trailing slash is needed
      RemoveSlash  // Indicates that a redirection to remove a trailing slash is needed
    };

    // Only one of them will be non null if found
    const RequestHandler* pRequestHandler{nullptr};
    const StreamingHandler* pStreamingHandler{nullptr};

    RedirectSlashMode redirectPathIndicator{RedirectSlashMode::None};

    bool methodNotAllowed{false};

    // Captured path parameters for the matched route, if any.
    // The span is valid until next call to match() on the same Router instance.
    std::span<const PathParamCapture> pathParams;

    // If set, points to the per-route CorsPolicy stored in the matched route entry; nullptr if none.
    const CorsPolicy* pCorsPolicy{nullptr};

    // The ordered range of RequestMiddleware to be applied.
    RequestMiddlewareRange requestMiddlewareRange;

    // The ordered range of ResponseMiddleware to be applied.
    ResponseMiddlewareRange responseMiddlewareRange;
  };

  // Match the provided `path` for `method` and return the matching handlers (or a
  // redirect indication or a method-not-allowed result).
  //
  // HEAD semantics: if no explicit HEAD handler is registered for a matching path,
  // the router will automatically fall back to the corresponding GET handler.
  //
  // Capture lifetime: `RoutingResult::pathParams` elements contain `string_view`s that
  // point into the caller-supplied path buffer and into the router's internal
  // transient storage. Callers must copy values if they need them to outlive the
  // original request buffer or a subsequent `match()` call which may mutate internal
  // buffers.
  [[nodiscard]] RoutingResult match(http::Method method, std::string_view path);

  // Return a bitmap of allowed HTTP methods for `path`.
  //
  // Semantics:
  //  - The path is normalized according to the router's trailing-slash policy before lookup
  //    (for example, `Normalize` will accept a trailing slash and prefer the variant that
  //    actually has registered handlers).
  //  - If a route node matches the provided path, the method bitmap is constructed from
  //    the registered normal and streaming handlers for the variant of the route that is
  //    appropriate for the requested trailing-slash form (see `RouterConfig::TrailingSlashPolicy`).
  //  - HEAD fallback: `allowedMethods` reports methods exactly as registered; it does not
  //    synthesize HEAD from GET. (A call to `match()` applies the HEAD->GET fallback when
  //    dispatching handlers.)
  //  - If no path-specific handlers match but a global handler (normal or streaming) is
  //    installed via `setDefault`, all methods are considered allowed (returns a bitmap with
  //    all method bits set).
  //  - If no match and no global handler, returns an empty bitmap (0).
  [[nodiscard]] http::MethodBmp allowedMethods(std::string_view path);

  // Clear all registered routes and handlers from the router.
  // The configuration stays unchanged.
  void clear() noexcept;

 private:
  struct SegmentPart {
    enum class Kind : std::uint8_t { Literal, Param };

    [[nodiscard]] Kind kind() const noexcept { return literal.empty() ? Kind::Param : Kind::Literal; }

    bool operator==(const SegmentPart&) const noexcept = default;

    using trivially_relocatable = amc::is_trivially_relocatable<SmallRawChars>::type;

    SmallRawChars literal;  // non empty when Kind::Literal
  };

  struct CompiledSegment {
    enum class Type : std::uint8_t { Literal, Pattern };

    [[nodiscard]] Type type() const noexcept { return literal.empty() ? Type::Pattern : Type::Literal; }

    bool operator==(const CompiledSegment&) const noexcept = default;

    using trivially_relocatable = std::true_type;

    SmallRawChars literal;      // non empty when Type::Literal
    vector<SegmentPart> parts;  // used when Type::Pattern
  };

  struct CompiledRoute {
    using trivially_relocatable = std::true_type;

    vector<CompiledSegment> segments;
    SmallConcatenatedStrings paramNames;
    bool hasWildcard{false};
    bool hasNoSlashRegistered{false};
    bool hasWithSlashRegistered{false};
  };

  struct RouteNode;

  struct DynamicEdge {
    using trivially_relocatable = std::true_type;

    CompiledSegment segment;
    RouteNode* child{nullptr};
  };

  using RouteNodeMap = flat_hash_map<SmallRawChars, RouteNode*, std::hash<std::string_view>, std::equal_to<>>;

  struct RouteNode {
    // Return a human-readable pattern string reconstructed from the compiled route
    // e.g. "/users/{param}/files/*" or "<empty>" when no route present.
    [[nodiscard]] SmallRawChars patternString() const;

    RouteNodeMap literalChildren;
    vector<DynamicEdge> dynamicChildren;
    RouteNode* wildcardChild{nullptr};

    PathHandlerEntry handlersNoSlash;
    PathHandlerEntry handlersWithSlash;
    CompiledRoute* route{nullptr};
  };

  PathHandlerEntry& setPathInternal(http::MethodBmp methods, std::string_view path, RequestHandler handler,
                                    StreamingHandler streaming);

  static CompiledRoute CompilePattern(std::string_view path);

  RouteNode* ensureLiteralChild(RouteNode& node, std::string_view segmentLiteral);
  RouteNode* ensureDynamicChild(RouteNode& node, const CompiledSegment& segmentPattern);

  static void AssignHandlers(RouteNode& node, http::MethodBmp methods, RequestHandler requestHandler,
                             StreamingHandler streamingHandler, bool registeredWithTrailingSlash);

  void ensureRouteMetadata(RouteNode& node, CompiledRoute&& route);

  bool matchPatternSegment(const CompiledSegment& segmentPattern, std::string_view segmentValue);

  bool matchImpl(bool requestHasTrailingSlash, const RouteNode*& matchedNode);

  bool matchWithWildcard(const RouteNode& node, bool requestHasTrailingSlash, const RouteNode*& matchedNode) const;

  void splitPathSegments(std::string_view path);

  const PathHandlerEntry* computePathHandlerEntry(const RouteNode& matchedNode, bool pathHasTrailingSlash,
                                                  RoutingResult::RedirectSlashMode& redirectSlashMode) const;

  void setMatchedHandler(http::Method method, const PathHandlerEntry& entry, RoutingResult& result) const;

  void cloneNodesFrom(const Router& other);

  struct StackFrame {
    const RouteNode* node;
    uint32_t segmentIndex;
    uint32_t dynamicChildIdx;
    uint32_t matchStateSize;
  };

  RouterConfig _config;

  RequestHandler _handler;
  StreamingHandler _streamingHandler;

  vector<RequestMiddleware> _globalPreMiddleware;
  vector<ResponseMiddleware> _globalPostMiddleware;

  ObjectPool<RouteNode> _nodePool;
  ObjectPool<CompiledRoute> _compiledRoutePool;
  RouteNode* _pRootRouteNode{nullptr};

  // Fast-path optimization: O(1) lookup for literal-only routes (no patterns, no wildcards).
  // Keys are normalized paths (trailing slash handled according to policy).
  // This avoids segment splitting and trie traversal for the common case of static routes.
  RouteNodeMap _literalOnlyRoutes;

  // Temporary buffers used during matching; reused across match() calls to minimize allocations.
  vector<PathParamCapture> _pathParamCaptureBuffer;
  vector<std::string_view> _matchStateBuffer;
  vector<std::string_view> _segmentBuffer;
  vector<StackFrame> _stackBuffer;
};

}  // namespace aeronet
