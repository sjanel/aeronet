#include "aeronet/router.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "aeronet/cors-policy.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/log.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handler-entry.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/stringconv.hpp"

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-endpoint.hpp"
#endif

namespace aeronet {
namespace {

constexpr std::string_view kEscapedOpenBrace = "{{";
constexpr std::string_view kEscapedCloseBrace = "}}";

bool MayNormalizeHasTrailingSlash(RouterConfig::TrailingSlashPolicy policy, std::string_view& path) {
  const auto sz = path.size();
  if (sz == 0) {
    throw std::invalid_argument("Path cannot be empty");
  }
  const bool pathHasTrailingSlash = sz > 1U && path.back() == '/';
  if (pathHasTrailingSlash && (policy == RouterConfig::TrailingSlashPolicy::Normalize ||
                               policy == RouterConfig::TrailingSlashPolicy::Redirect)) {
    path.remove_suffix(1U);
  }
  return pathHasTrailingSlash;
}

}  // namespace

Router::Router(RouterConfig config) : _config(std::move(config)) {}

Router::Router(Router&&) noexcept = default;
Router& Router::operator=(Router&&) noexcept = default;

Router::Router(const Router& other)
    : _config(other._config),
      _handler(other._handler),
      _asyncHandler(other._asyncHandler),
      _streamingHandler(other._streamingHandler),
      _globalPreMiddleware(other._globalPreMiddleware),
      _globalPostMiddleware(other._globalPostMiddleware) {
  cloneNodesFrom(other);
}

Router& Router::operator=(const Router& other) {
  if (this != &other) {
    _config = other._config;
    _handler = other._handler;
    _asyncHandler = other._asyncHandler;
    _streamingHandler = other._streamingHandler;
    _globalPreMiddleware = other._globalPreMiddleware;
    _globalPostMiddleware = other._globalPostMiddleware;

    cloneNodesFrom(other);
  }
  return *this;
}

Router::~Router() = default;

void Router::addRequestMiddleware(RequestMiddleware middleware) {
  _globalPreMiddleware.emplace_back(std::move(middleware));
}

void Router::addResponseMiddleware(ResponseMiddleware middleware) {
  _globalPostMiddleware.emplace_back(std::move(middleware));
}

void Router::splitPathSegments(std::string_view path) {
  _segmentBuffer.clear();

  std::size_t pos = path.front() == '/' ? 1U : 0U;
  if (pos >= path.size()) {
    return;
  }

  while (pos < path.size()) {
    const std::size_t nextSlash = path.find('/', pos);
    if (nextSlash == std::string_view::npos) {
      _segmentBuffer.push_back(path.substr(pos));
      break;
    }
    _segmentBuffer.push_back(path.substr(pos, nextSlash - pos));
    pos = nextSlash + 1U;
  }
}

void Router::setDefault(RequestHandler handler) {
  _handler = std::move(handler);
  if (_streamingHandler) {
    log::warn("Overwriting existing default streaming handler with normal handler");
    _streamingHandler = {};
  }
}

void Router::setDefault(AsyncRequestHandler handler) { _asyncHandler = std::move(handler); }

void Router::setDefault(StreamingHandler handler) {
  _streamingHandler = std::move(handler);
  if (_handler) {
    log::warn("Overwriting existing default handler with streaming handler");
    _handler = {};
  }
}

PathHandlerEntry& Router::setPath(http::MethodBmp methods, std::string_view path, RequestHandler handler) {
  return setPathInternal(methods, path, std::move(handler));
}

PathHandlerEntry& Router::setPath(http::Method method, std::string_view path, RequestHandler handler) {
  return setPathInternal(static_cast<http::MethodBmp>(method), path, std::move(handler));
}

PathHandlerEntry& Router::setPath(http::MethodBmp methods, std::string_view path, StreamingHandler handler) {
  return setPathInternal(methods, path, std::move(handler));
}

PathHandlerEntry& Router::setPath(http::Method method, std::string_view path, StreamingHandler handler) {
  return setPathInternal(static_cast<http::MethodBmp>(method), path, std::move(handler));
}

PathHandlerEntry& Router::setPath(http::MethodBmp methods, std::string_view path, AsyncRequestHandler handler) {
  return setPathInternal(methods, path, std::move(handler));
}

PathHandlerEntry& Router::setPath(http::Method method, std::string_view path, AsyncRequestHandler handler) {
  return setPathInternal(static_cast<http::MethodBmp>(method), path, std::move(handler));
}

#ifdef AERONET_ENABLE_WEBSOCKET
PathHandlerEntry& Router::setWebSocket(std::string_view path, WebSocketEndpoint endpoint) {
  return setPathInternal({}, path, std::move(endpoint));
}
#endif

PathHandlerEntry& Router::setPathInternal(http::MethodBmp methods, std::string_view path,
                                          HandlerVariant handlerVariant) {
  const bool pathHasTrailingSlash = MayNormalizeHasTrailingSlash(_config.trailingSlashPolicy, path);

  CompiledRoute compiled = CompilePattern(path);
  if (pathHasTrailingSlash) {
    compiled.hasWithSlashRegistered = true;
  } else {
    compiled.hasNoSlashRegistered = true;
  }

  if (_pRootRouteNode == nullptr) {
    _pRootRouteNode = _nodePool.allocateAndConstruct();
  }

  // Insert into trie for pattern matching (also for literal-only routes, for simpler cloning)
  // For literal-only routes, we'll also store a pointer in _literalOnlyRoutes for O(1) fast-path lookup
  RouteNode* pNode = _pRootRouteNode;
  for (const auto& segment : compiled.segments) {
    if (segment.type() == CompiledSegment::Type::Literal) {
      pNode = ensureLiteralChild(*pNode, segment.literal);
    } else {
      pNode = ensureDynamicChild(*pNode, segment);
    }
  }

  if (compiled.hasWildcard) {
    if (pNode->wildcardChild == nullptr) {
      pNode->wildcardChild = _nodePool.allocateAndConstruct();
    }
    pNode = pNode->wildcardChild;
  }

  // If this is a literal-only route, also store it in the fast-path map for O(1) lookup
  if (compiled.paramNames.empty() && !compiled.hasWildcard) {
    _literalOnlyRoutes[RawChars32(path)] = pNode;
  }

  ensureRouteMetadata(*pNode, std::move(compiled));

  // Assign the handler based on the variant type using a visitor for clarity
  PathHandlerEntry& entry = pathHasTrailingSlash ? pNode->handlersWithSlash : pNode->handlersNoSlash;

  std::visit(
      [&entry, methods, pNode](auto&& handler) {
        using T = std::decay_t<decltype(handler)>;
        if constexpr (std::is_same_v<T, RequestHandler>) {
          if (!handler) {
            throw std::invalid_argument("Cannot set empty RequestHandler");
          }
          if ((entry._normalMethodBmp & methods) != 0) {
            log::warn("Overwriting existing path handler for {}", std::string_view(pNode->patternString()));
          }
          entry.assignNormalHandler(methods, std::forward<decltype(handler)>(handler));
        } else if constexpr (std::is_same_v<T, StreamingHandler>) {
          if (!handler) {
            throw std::invalid_argument("Cannot set empty StreamingHandler");
          }
          if ((entry._streamingMethodBmp & methods) != 0) {
            log::warn("Overwriting existing streaming path handler for {}", std::string_view(pNode->patternString()));
          }
          entry.assignStreamingHandler(methods, std::forward<decltype(handler)>(handler));
        } else if constexpr (std::is_same_v<T, AsyncRequestHandler>) {
          if (!handler) {
            throw std::invalid_argument("Cannot set empty AsyncRequestHandler");
          }
          if ((entry._asyncMethodBmp & methods) != 0) {
            log::warn("Overwriting existing async path handler for {}", std::string_view(pNode->patternString()));
          }
          entry.assignAsyncHandler(methods, std::forward<decltype(handler)>(handler));
#ifdef AERONET_ENABLE_WEBSOCKET
        } else if constexpr (std::is_same_v<T, WebSocketEndpoint>) {
          entry.assignWebSocketEndpoint(std::forward<decltype(handler)>(handler));
#endif
        } else {
          static_assert(false, "Non-exhaustive visitor!");
        }
      },
      std::move(handlerVariant));

  return entry;
}

Router::RouteNode* Router::ensureLiteralChild(RouteNode& node, std::string_view segmentLiteral) {
  auto it = node.literalChildren.find(segmentLiteral);
  if (it != node.literalChildren.end()) {
    return it->second;
  }
  RouteNode* child = _nodePool.allocateAndConstruct();
  node.literalChildren.emplace(segmentLiteral, child);
  return child;
}

Router::RouteNode* Router::ensureDynamicChild(RouteNode& node, const CompiledSegment& segmentPattern) {
  const auto it = std::ranges::find_if(
      node.dynamicChildren, [&segmentPattern](const DynamicEdge& edge) { return edge.segment == segmentPattern; });
  if (it != node.dynamicChildren.end()) {
    return it->child;
  }
  return node.dynamicChildren.emplace_back(segmentPattern, _nodePool.allocateAndConstruct()).child;
}

void Router::ensureRouteMetadata(RouteNode& node, CompiledRoute&& route) {
  if (node.pRoute == nullptr) {
    node.pRoute = _compiledRoutePool.allocateAndConstruct(std::move(route));
    return;
  }

  const CompiledRoute& existing = *node.pRoute;
  if (existing.paramNames != route.paramNames) {
    throw std::logic_error("Conflicting parameter naming for identical path pattern");
  }
  // both hasWildcard should be the same as well
  node.pRoute->hasNoSlashRegistered |= route.hasNoSlashRegistered;
  node.pRoute->hasWithSlashRegistered |= route.hasWithSlashRegistered;
}

bool Router::matchPatternSegment(const CompiledSegment& segmentPattern, std::string_view segmentValue) {
  assert(segmentPattern.type() == CompiledSegment::Type::Pattern);
  std::size_t pos = 0;
  for (uint32_t idx = 0; idx < segmentPattern.parts.size(); ++idx) {
    const SegmentPart& part = segmentPattern.parts[idx];
    if (part.kind() == SegmentPart::Kind::Literal) {
      assert(pos <= segmentValue.size());
      if (!segmentValue.substr(pos).starts_with(part.literal)) {
        return false;
      }
      pos += part.literal.size();
      continue;
    }

    const auto captureStart = pos;
    auto captureEnd = segmentValue.size();
    if (idx + 1 < segmentPattern.parts.size()) {
      const SegmentPart& next = segmentPattern.parts[idx + 1];
      assert(next.kind() == SegmentPart::Kind::Literal);
      const std::size_t found = segmentValue.find(next.literal, pos);
      if (found == std::string_view::npos) {
        return false;
      }
      captureEnd = found;
      pos = found;
    } else {
      pos = segmentValue.size();
    }

    _matchStateBuffer.push_back(segmentValue.substr(captureStart, captureEnd - captureStart));
  }

  return pos == segmentValue.size();
}

const Router::RouteNode* Router::matchWithWildcard(const RouteNode& node, bool requestHasTrailingSlash) const {
  const RouteNode* pWildcardNode = node.wildcardChild;
  if (pWildcardNode == nullptr) {
    return nullptr;
  }
  assert(pWildcardNode->pRoute != nullptr);
  if (_config.trailingSlashPolicy == RouterConfig::TrailingSlashPolicy::Strict) {
    if (requestHasTrailingSlash) {
      // Because compile pattern rejects paths which don't have a terminating wildcard
      assert(!pWildcardNode->pRoute->hasWithSlashRegistered);
      return nullptr;
    }
    assert(pWildcardNode->pRoute->hasNoSlashRegistered);
  }
  return pWildcardNode;
}

RawChars32 Router::RouteNode::patternString() const {
  static constexpr std::string_view kParam = "{param}";
  static constexpr std::string_view kSlashAsterisk = "/*";

  // Not possible through the public API
  assert(pRoute != nullptr);

  std::size_t sz = 0;
  for (const auto& seg : pRoute->segments) {
    sz += 1U;
    sz += seg.literal.empty() ? kParam.size() : seg.literal.size();
  }
  if (pRoute->hasWildcard) {
    sz += kSlashAsterisk.size();
  } else if (sz == 0U) {
    sz = 1U;
  }

  RawChars32 out(sz);
  for (const auto& seg : pRoute->segments) {
    out.unchecked_push_back('/');
    out.unchecked_append(seg.literal.empty() ? kParam : seg.literal);
  }
  if (pRoute->hasWildcard) {
    out.unchecked_append(kSlashAsterisk);
  } else if (out.empty()) {
    out.unchecked_push_back('/');
  }
  return out;
}

const Router::RouteNode* Router::matchImpl(bool requestHasTrailingSlash) {
  if (_pRootRouteNode == nullptr) {
    return nullptr;
  }

  _matchStateBuffer.clear();
  _stackBuffer.clear();

  // DFS
  for (_stackBuffer.emplace_back(_pRootRouteNode, 0, 0, 0); !_stackBuffer.empty();) {
    StackFrame frame = std::move(_stackBuffer.back());
    _stackBuffer.pop_back();

    // Terminal: all segments matched
    if (frame.segmentIndex == _segmentBuffer.size()) {
      if (frame.node->pRoute != nullptr) {
        if (_config.trailingSlashPolicy != RouterConfig::TrailingSlashPolicy::Strict) {
          return frame.node;
        }
        if (requestHasTrailingSlash ? frame.node->pRoute->hasWithSlashRegistered
                                    : frame.node->pRoute->hasNoSlashRegistered) {
          return frame.node;
        }
      }
      if (const RouteNode* wildcardMatch = matchWithWildcard(*frame.node, requestHasTrailingSlash)) {
        return wildcardMatch;
      }
      continue;
    }

    const std::string_view segment = _segmentBuffer[frame.segmentIndex];

    // Try literal child (only on first visit to this frame)
    if (frame.dynamicChildIdx == 0) {
      // Mark that we've tried literal
      ++frame.dynamicChildIdx;

      const auto it = frame.node->literalChildren.find(segment);
      if (it != frame.node->literalChildren.end()) {
        // Push current frame back for later retry, then push child frame
        _stackBuffer.push_back(frame);
        _stackBuffer.emplace_back(it->second, frame.segmentIndex + 1, 0,
                                  static_cast<uint32_t>(_matchStateBuffer.size()));
        continue;
      }
    }

    // Try dynamic children
    const uint32_t dynamicCount = static_cast<uint32_t>(frame.node->dynamicChildren.size());
    const uint32_t edgeIdx = frame.dynamicChildIdx - 1;
    if (edgeIdx < dynamicCount) {
      const auto& edge = frame.node->dynamicChildren[edgeIdx];
      ++frame.dynamicChildIdx;

      _matchStateBuffer.resize(frame.matchStateSize);
      if (matchPatternSegment(edge.segment, segment)) {
        // Push updated frame back, then push child
        _stackBuffer.push_back(frame);
        _stackBuffer.emplace_back(edge.child, frame.segmentIndex + 1, 0,
                                  static_cast<uint32_t>(_matchStateBuffer.size()));
        continue;
      }
      // This edge didn't match, push frame back to try next edge
      _stackBuffer.push_back(std::move(frame));
      continue;
    }

    // All children exhausted, try wildcard
    _matchStateBuffer.resize(frame.matchStateSize);
    const RouteNode* matchedNode = matchWithWildcard(*frame.node, requestHasTrailingSlash);
    if (matchedNode != nullptr) {
      return matchedNode;
    }
    // No match found, backtrack (frame already popped)
  }

  return nullptr;
}

Router::RoutingResult Router::match(http::Method method, std::string_view path) {
  const bool pathHasTrailingSlash = MayNormalizeHasTrailingSlash(_config.trailingSlashPolicy, path);

  RoutingResult result;

  // Fast path: O(1) lookup for literal-only routes
  if (const auto it = _literalOnlyRoutes.find(path); it != _literalOnlyRoutes.end()) {
    const RouteNode* matchedNode = it->second;
    // Choose which handler entry to use based on trailing slash policy
    const PathHandlerEntry* entryPtr =
        computePathHandlerEntry(*matchedNode, pathHasTrailingSlash, result.redirectPathIndicator);
    if (entryPtr == nullptr) {
      return result;
    }

    setMatchedHandler(method, *entryPtr, result);

    // No path params for literal-only routes
    return result;
  }

  // Slow path: split segments and match patterns via trie
  splitPathSegments(path);

  const RouteNode* pMatchedNode = matchImpl(pathHasTrailingSlash);
  if (pMatchedNode == nullptr) {
    if (_streamingHandler) {
      result.setStreamingHandler(&_streamingHandler);
    } else if (_asyncHandler) {
      result.setAsyncRequestHandler(&_asyncHandler);
    } else if (_handler) {
      result.setRequestHandler(&_handler);
    }
    return result;
  }

  const CompiledRoute* route = pMatchedNode->pRoute;

  // Choose which handler entry to use depending on the trailing slash policy.
  const PathHandlerEntry* entryPtr =
      computePathHandlerEntry(*pMatchedNode, pathHasTrailingSlash, result.redirectPathIndicator);
  if (entryPtr == nullptr) {
    return result;
  }

  setMatchedHandler(method, *entryPtr, result);

  assert(std::cmp_equal(route->paramNames.nbConcatenatedStrings(), _matchStateBuffer.size()));

  _pathParamCaptureBuffer.clear();
  for (auto [paramPos, param] : std::views::enumerate(route->paramNames)) {
    _pathParamCaptureBuffer.emplace_back(param, _matchStateBuffer[static_cast<uint32_t>(paramPos)]);
  }

  result.pathParams = _pathParamCaptureBuffer;

  return result;
}

http::MethodBmp Router::allowedMethods(std::string_view path) {
  const bool pathHasTrailingSlash = MayNormalizeHasTrailingSlash(_config.trailingSlashPolicy, path);

  // Fast path: O(1) lookup for literal-only routes
  if (const auto it = _literalOnlyRoutes.find(path); it != _literalOnlyRoutes.end()) {
    const RouteNode* pMatchedNode = it->second;
    const auto& entry = pathHasTrailingSlash ? pMatchedNode->handlersWithSlash : pMatchedNode->handlersNoSlash;
    return static_cast<http::MethodBmp>(entry._normalMethodBmp | entry._streamingMethodBmp | entry._asyncMethodBmp);
  }

  // Slow path: split segments and match patterns via trie
  splitPathSegments(path);

  const RouteNode* pMatchedNode = matchImpl(pathHasTrailingSlash);
  if (pMatchedNode != nullptr) {
    const auto& entry = pathHasTrailingSlash ? pMatchedNode->handlersWithSlash : pMatchedNode->handlersNoSlash;
    return static_cast<http::MethodBmp>(entry._normalMethodBmp | entry._streamingMethodBmp | entry._asyncMethodBmp);
  }

  if (_streamingHandler || _handler || _asyncHandler) {
    static constexpr http::MethodBmp kAllMethods = (1U << http::kNbMethods) - 1U;
    return kAllMethods;
  }
  return 0U;
}

Router::CompiledRoute Router::CompilePattern(std::string_view path) {
  if (path.front() != '/') {
    throw std::invalid_argument("Router paths must begin with '/'");
  }

  CompiledRoute route;
  bool sawNamed = false;
  bool sawUnnamed = false;

  uint32_t paramIdx = 0;
  for (std::size_t pos = 1U; pos < path.size();) {
    const std::size_t nextSlash = path.find('/', pos);
    const std::string_view segment =
        nextSlash == std::string_view::npos ? path.substr(pos) : path.substr(pos, nextSlash - pos);

    if (segment.empty()) {
      throw std::invalid_argument("Router path contains empty segment");
    }

    if (segment == "*") {
      if (nextSlash != std::string_view::npos) {
        throw std::invalid_argument("Wildcard segment must be terminal");
      }
      route.hasWildcard = true;
      break;
    }

    CompiledSegment compiledSegment;

    if (!segment.contains('{')) {
      compiledSegment.literal.assign(segment);
      route.segments.push_back(std::move(compiledSegment));
      if (nextSlash == std::string_view::npos) {
        break;
      }
      pos = nextSlash + 1U;
      continue;
    }

    RawChars32 literalBuffer;
    bool previousWasParam = false;
    bool hasParam = false;

    for (std::size_t i = 0; i < segment.size();) {
      if (segment.compare(i, kEscapedOpenBrace.size(), kEscapedOpenBrace) == 0) {
        literalBuffer.push_back('{');
        i += kEscapedOpenBrace.size();
        continue;
      }
      if (segment.compare(i, kEscapedCloseBrace.size(), kEscapedCloseBrace) == 0) {
        literalBuffer.push_back('}');
        i += kEscapedCloseBrace.size();
        continue;
      }
      if (segment[i] != '{') {
        literalBuffer.push_back(segment[i]);
        ++i;
        continue;
      }

      const std::size_t closePos = segment.find('}', i + 1U);
      if (closePos == std::string_view::npos) {
        throw std::invalid_argument("Unterminated '{' in router pattern");
      }

      if (!literalBuffer.empty()) {
        SegmentPart literalPart{std::move(literalBuffer)};
        // It's fine to call clear after a move.
        literalBuffer.clear();  // NOLINT(bugprone-use-after-move)
        compiledSegment.parts.push_back(std::move(literalPart));
        previousWasParam = false;
      }

      compiledSegment.parts.emplace_back();

      if (previousWasParam) {
        throw std::invalid_argument("Consecutive parameters without separator are not allowed");
      }
      previousWasParam = true;
      hasParam = true;

      const std::string_view paramName = segment.substr(i + 1U, closePos - i - 1U);
      if (paramName.empty()) {
        sawUnnamed = true;
        route.paramNames.append(std::string_view(IntegralToCharVector(paramIdx)));
      } else {
        sawNamed = true;
        route.paramNames.append(paramName);
      }

      ++paramIdx;

      i = closePos + 1U;
    }

    if (!literalBuffer.empty()) {
      SegmentPart literalPart{std::move(literalBuffer)};
      // It's fine to call clear after a move.
      literalBuffer.clear();  // NOLINT(bugprone-use-after-move)
      compiledSegment.parts.push_back(std::move(literalPart));
      previousWasParam = false;
    }

    if (!hasParam) {
      compiledSegment.literal.assign(segment);
      compiledSegment.parts.clear();
    }

    route.segments.push_back(std::move(compiledSegment));

    if (nextSlash == std::string_view::npos) {
      break;
    }
    pos = nextSlash + 1U;
  }

  if (sawNamed && sawUnnamed) {
    throw std::invalid_argument("Cannot mix named and unnamed parameters in a single path pattern");
  }

  return route;
}

const PathHandlerEntry* Router::computePathHandlerEntry(const RouteNode& matchedNode, bool pathHasTrailingSlash,
                                                        RoutingResult::RedirectSlashMode& redirectSlashMode) const {
  switch (_config.trailingSlashPolicy) {
    case RouterConfig::TrailingSlashPolicy::Strict:
      return pathHasTrailingSlash ? &matchedNode.handlersWithSlash : &matchedNode.handlersNoSlash;
    case RouterConfig::TrailingSlashPolicy::Normalize: {
      // Normalize accepts both variants. Prefer the variant that actually has handlers
      // for the requested form, otherwise fall back to the other variant.
      const auto& matchedSlash = pathHasTrailingSlash ? matchedNode.handlersWithSlash : matchedNode.handlersNoSlash;
      if (matchedSlash.hasAnyHandler()) {
        return &matchedSlash;
      }
      return pathHasTrailingSlash ? &matchedNode.handlersNoSlash : &matchedNode.handlersWithSlash;
    }
    case RouterConfig::TrailingSlashPolicy::Redirect:
      // If only the opposite-slashed variant is registered, tell the caller to redirect.
      if (pathHasTrailingSlash) {
        if (matchedNode.handlersWithSlash.hasAnyHandler()) {
          return &matchedNode.handlersWithSlash;
        }
        assert(matchedNode.handlersNoSlash.hasAnyHandler());
        redirectSlashMode = RoutingResult::RedirectSlashMode::RemoveSlash;
      } else {
        if (matchedNode.handlersNoSlash.hasAnyHandler()) {
          return &matchedNode.handlersNoSlash;
        }
        assert(matchedNode.handlersWithSlash.hasAnyHandler());
        redirectSlashMode = RoutingResult::RedirectSlashMode::AddSlash;
      }
      break;
  }
  return nullptr;
}

void Router::setMatchedHandler(http::Method method, const PathHandlerEntry& entry, RoutingResult& result) const {
  auto methodIdx = MethodToIdx(method);

  if (method == http::Method::HEAD) {
    static constexpr http::MethodIdx kGetIdx = MethodToIdx(http::Method::GET);
    static constexpr http::MethodIdx kHeadIdx = MethodToIdx(http::Method::HEAD);
    if (!entry.hasNormalHandler(kHeadIdx) && !entry.hasStreamingHandler(kHeadIdx) && !entry.hasAsyncHandler(kHeadIdx)) {
      if (entry.hasNormalHandler(kGetIdx) || entry.hasStreamingHandler(kGetIdx) || entry.hasAsyncHandler(kGetIdx)) {
        method = http::Method::GET;
        methodIdx = kGetIdx;
      }
    }
  }

  if (entry.hasStreamingHandler(methodIdx)) {
    assert(http::IsMethodSet(entry._streamingMethodBmp, method));
    result.setStreamingHandler(entry.streamingHandlerPtr(methodIdx));
  } else if (entry.hasAsyncHandler(methodIdx)) {
    assert(http::IsMethodSet(entry._asyncMethodBmp, method));
    result.setAsyncRequestHandler(entry.asyncHandlerPtr(methodIdx));
  } else if (entry.hasNormalHandler(methodIdx)) {
    assert(http::IsMethodSet(entry._normalMethodBmp, method));
    result.setRequestHandler(entry.requestHandlerPtr(methodIdx));
#ifdef AERONET_ENABLE_WEBSOCKET
  } else if (entry.hasWebSocketEndpoint() && method == http::Method::GET) {
// WebSocket endpoint on GET - handler will be determined by upgrade validation
// Don't mark as methodNotAllowed; let the WebSocket upgrade code handle it
#endif
  } else {
    result.methodNotAllowed = true;
  }

  // Expose per-route cors policy pointer if present
  if (entry._corsPolicy.active()) {
    result.pCorsPolicy = &entry._corsPolicy;
  } else if (_config.defaultCorsPolicy.active()) {
    result.pCorsPolicy = &_config.defaultCorsPolicy;
  }

#ifdef AERONET_ENABLE_WEBSOCKET
  // Expose WebSocket endpoint if present
  if (entry.hasWebSocketEndpoint()) {
    result.pWebSocketEndpoint = entry.webSocketEndpointPtr();
  }
#endif

  result.requestMiddlewareRange = entry._preMiddleware;
  result.responseMiddlewareRange = entry._postMiddleware;
}

void Router::cloneNodesFrom(const Router& other) {
  // Clone strategy:
  // 1. Copy compiled routes and build a mapping (routeMap) for pointer remapping
  // 2. Traverse the trie tree (starting from _root) and clone all RouteNode objects
  // 3. Build a nodeMap during trie traversal to track old→new node pointer mappings
  // 4. Clone _literalOnlyRoutes by remapping pointers via nodeMap
  //
  // Key simplification: ALL routes (including literal-only) are stored in the trie.
  // The _literalOnlyRoutes map is just a fast-path index pointing to trie nodes,
  // so we don't need to separately clone those nodes - they're already cloned during
  // trie traversal. We only need to remap the pointers in the fast-path map.

  _nodePool.clear();

  flat_hash_map<const CompiledRoute*, CompiledRoute*> routeMap;

  // Clear our compiled route pool so we can clone routes from `other` into it.
  _compiledRoutePool.clear();

  // Collect unique compiled route pointers from the source trie by traversing it.
  vector<const RouteNode*> nodesToVisit;
  if (other._pRootRouteNode != nullptr) {
    nodesToVisit.push_back(other._pRootRouteNode);
  }
  flat_hash_map<const CompiledRoute*, bool> seen;

  while (!nodesToVisit.empty()) {
    const RouteNode* nodePtr = nodesToVisit.back();
    nodesToVisit.pop_back();

    if (nodePtr->pRoute != nullptr) {
      [[maybe_unused]] const auto [it, inserted] = seen.emplace(nodePtr->pRoute, true);
      assert(inserted);

      // Clone the compiled route into our pool
      CompiledRoute* cloned = _compiledRoutePool.allocateAndConstruct(*nodePtr->pRoute);
      routeMap.emplace(nodePtr->pRoute, cloned);
    }

    for (const auto& [seg, child] : nodePtr->literalChildren) {
      assert(child != nullptr);
      nodesToVisit.push_back(child);
    }
    for (const auto& edge : nodePtr->dynamicChildren) {
      assert(edge.child != nullptr);
      nodesToVisit.push_back(edge.child);
    }
    if (nodePtr->wildcardChild != nullptr) {
      nodesToVisit.push_back(nodePtr->wildcardChild);
    }
  }

  // Build a map from old RouteNode* to new RouteNode* so we can remap fast-path pointers
  flat_hash_map<const RouteNode*, RouteNode*> nodeMap;

  // Clone directly into this router's root so that child nodes are allocated from our pool.
  // Clear any existing children in _root by reassigning a default RouteNode and let the
  // pool manage newly allocated children.
  const RouteNode* pSource = other._pRootRouteNode;

  struct Node {
    const RouteNode* srcNode;
    RouteNode* dstNode;
  };

  vector<Node> workQueue;

  if (pSource != nullptr) {
    _pRootRouteNode = _nodePool.allocateAndConstruct();
    workQueue.emplace_back(pSource, _pRootRouteNode);
  } else {
    _pRootRouteNode = nullptr;
  }

  while (!workQueue.empty()) {
    const Node node = std::move(workQueue.back());
    workQueue.pop_back();

    const RouteNode* src = node.srcNode;
    RouteNode* dst = node.dstNode;

    // Record the mapping from old node to new node for fast-path cloning
    nodeMap.emplace(src, dst);

    // Copy handler entries
    dst->handlersNoSlash = src->handlersNoSlash;
    dst->handlersWithSlash = src->handlersWithSlash;

    // Map compiled route pointer
    if (src->pRoute != nullptr) {
      const auto it = routeMap.find(src->pRoute);
      assert(it != routeMap.end());
      dst->pRoute = it->second;
    } else {
      dst->pRoute = nullptr;
    }

    // Clone literal children
    dst->literalChildren.clear();
    dst->literalChildren.reserve(src->literalChildren.size());
    for (const auto& [segment, childPtr] : src->literalChildren) {
      assert(childPtr != nullptr);
      RouteNode* newChild = _nodePool.allocateAndConstruct();
      dst->literalChildren.emplace(segment, newChild);
      workQueue.emplace_back(childPtr, newChild);
    }

    // Clone dynamic children
    dst->dynamicChildren.clear();
    dst->dynamicChildren.reserve(src->dynamicChildren.size());
    for (const auto& edge : src->dynamicChildren) {
      assert(edge.child != nullptr);
      RouteNode* newChild = _nodePool.allocateAndConstruct();
      dst->dynamicChildren.push_back({edge.segment, newChild});
      workQueue.emplace_back(edge.child, newChild);
    }

    // Clone wildcard child
    if (src->wildcardChild != nullptr) {
      dst->wildcardChild = _nodePool.allocateAndConstruct();
      workQueue.emplace_back(src->wildcardChild, dst->wildcardChild);
    } else {
      dst->wildcardChild = nullptr;
    }
  }

  // Clone the fast-path literal-only routes map
  // Since all literal-only nodes are now also in the trie, they've already been cloned above.
  // We just need to remap the pointers using the nodeMap.
  _literalOnlyRoutes.clear();
  _literalOnlyRoutes.reserve(other._literalOnlyRoutes.size());
  for (const auto& [path, oldNode] : other._literalOnlyRoutes) {
    // Look up the cloned node in our mapping. This should always succeed because
    // all literal-only routes are present in the trie and were cloned above.
    const auto it = nodeMap.find(oldNode);
    assert(it != nodeMap.end());
    _literalOnlyRoutes.emplace(path, it->second);
  }
}

void Router::clear() noexcept {
  _handler = {};
  _asyncHandler = {};
  _streamingHandler = {};
  _globalPreMiddleware.clear();
  _globalPostMiddleware.clear();
  _nodePool.clear();
  _compiledRoutePool.clear();
  _pRootRouteNode = nullptr;
  _literalOnlyRoutes.clear();
}

}  // namespace aeronet
