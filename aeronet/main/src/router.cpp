#include "aeronet/router.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/http-method.hpp"
#include "aeronet/router-config.hpp"
#include "exception.hpp"
#include "flat-hash-map.hpp"
#include "log.hpp"

namespace aeronet {

namespace {

constexpr std::string_view kEscapedOpenBrace = "{{";
constexpr std::string_view kEscapedCloseBrace = "}}";

bool ShouldNormalize(RouterConfig::TrailingSlashPolicy policy, auto& path) noexcept {
  const bool pathHasTrailingSlash = path.size() > 1U && path.back() == '/';
  if (pathHasTrailingSlash && (policy == RouterConfig::TrailingSlashPolicy::Normalize ||
                               policy == RouterConfig::TrailingSlashPolicy::Redirect)) {
    if constexpr (std::is_same_v<decltype(path), std::string&>) {
      path.pop_back();
    } else {
      path.remove_suffix(1U);
    }
  }
  return pathHasTrailingSlash;
}

}  // namespace

Router::Router(RouterConfig config) : _config(std::move(config)) {}

Router::Router(const Router& other) { cloneFrom(other); }

Router& Router::operator=(const Router& other) {
  if (this != &other) {
    cloneFrom(other);
  }
  return *this;
}

void Router::splitPathSegments(std::string_view path) {
  _segmentBuffer.clear();
  if (path.empty()) {
    return;
  }

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
  if (_handler) {
    log::warn("Overwriting existing default request handler");
  }
  _handler = std::move(handler);
}

void Router::setDefault(StreamingHandler handler) {
  if (_streamingHandler) {
    log::warn("Overwriting existing default streaming handler");
  }
  _streamingHandler = std::move(handler);
}

void Router::setPath(http::MethodBmp methods, std::string path, RequestHandler handler) {
  setPathInternal(methods, std::move(path), std::move(handler), StreamingHandler{});
}

void Router::setPath(http::Method method, std::string path, RequestHandler handler) {
  setPath(static_cast<http::MethodBmp>(method), std::move(path), std::move(handler));
}

void Router::setPath(http::MethodBmp methods, std::string path, StreamingHandler handler) {
  setPathInternal(methods, std::move(path), RequestHandler{}, std::move(handler));
}

void Router::setPath(http::Method method, std::string path, StreamingHandler handler) {
  setPath(static_cast<http::MethodBmp>(method), std::move(path), std::move(handler));
}

void Router::setPathInternal(http::MethodBmp methods, std::string path, RequestHandler handler,
                             StreamingHandler streaming) {
  if (!handler && !streaming) {
    throw exception("setPath requires a handler");
  }

  const bool pathHasTrailingSlash = ShouldNormalize(_config.trailingSlashPolicy, path);

  CompiledRoute compiled = compilePattern(path);
  if (pathHasTrailingSlash) {
    compiled.hasWithSlashRegistered = true;
  } else {
    compiled.hasNoSlashRegistered = true;
  }

  // Check if this is a literal-only route (no patterns, no wildcard) for fast-path optimization
  const bool isLiteralOnly = compiled.paramNames.empty() && !compiled.hasWildcard;

  if (_pRootRouteNode == nullptr) {
    _pRootRouteNode = _nodePool.allocateAndConstruct();
  }

  // Insert into trie for pattern matching (also for literal-only routes, for simpler cloning)
  // For literal-only routes, we'll also store a pointer in _literalOnlyRoutes for O(1) fast-path lookup
  RouteNode* node = _pRootRouteNode;
  for (const auto& segment : compiled.segments) {
    if (segment.type() == CompiledSegment::Type::Literal) {
      node = ensureLiteralChild(*node, segment.literal);
    } else {
      node = ensureDynamicChild(*node, segment);
    }
  }

  if (compiled.hasWildcard) {
    if (node->wildcardChild == nullptr) {
      node->wildcardChild = _nodePool.allocateAndConstruct();
    }
    node = node->wildcardChild;
  }

  ensureRouteMetadata(*node, std::move(compiled));
  assignHandlers(*node, methods, std::move(handler), std::move(streaming), pathHasTrailingSlash);

  // If this is a literal-only route, also store it in the fast-path map for O(1) lookup
  if (isLiteralOnly) {
    _literalOnlyRoutes[path] = node;
  }
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

void Router::assignHandlers(RouteNode& node, http::MethodBmp methods, RequestHandler requestHandler,
                            StreamingHandler streamingHandler, bool registeredWithTrailingSlash) {
  PathHandlerEntry& entry = registeredWithTrailingSlash ? node.handlersWithSlash : node.handlersNoSlash;

  const bool hasNormalHandler = static_cast<bool>(requestHandler);
  const bool hasStreamingHandler = static_cast<bool>(streamingHandler);

  if (hasNormalHandler) {
    entry.normalMethodBmp |= methods;
  } else if (hasStreamingHandler) {
    entry.streamingMethodBmp |= methods;
  }

  const RequestHandler* pSharedRequest = nullptr;
  const StreamingHandler* pSharedStreaming = nullptr;

  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    if (!http::isMethodSet(methods, methodIdx)) {
      continue;
    }
    if (hasNormalHandler) {
      if (entry.streamingHandlers[methodIdx]) {
        throw exception("Cannot register normal handler: streaming handler already present for path+method");
      }
      if (entry.normalHandlers[methodIdx]) {
        log::warn("Overwriting existing path handler");
      }
      if (pSharedRequest == nullptr) {
        // We use at most once requestHandler, we then copy it to other methods
        // NOLINTNEXTLINE(bugprone-use-after-move)
        entry.normalHandlers[methodIdx] = std::move(requestHandler);
        pSharedRequest = &entry.normalHandlers[methodIdx];
      } else {
        entry.normalHandlers[methodIdx] = *pSharedRequest;
      }
    } else {
      if (entry.normalHandlers[methodIdx]) {
        throw exception("Cannot register streaming handler: normal handler already present for path+method");
      }
      if (entry.streamingHandlers[methodIdx]) {
        log::warn("Overwriting existing streaming path handler");
      }
      if (pSharedStreaming == nullptr) {
        // NOLINTNEXTLINE(bugprone-use-after-move)
        entry.streamingHandlers[methodIdx] = std::move(streamingHandler);
        pSharedStreaming = &entry.streamingHandlers[methodIdx];
      } else {
        entry.streamingHandlers[methodIdx] = *pSharedStreaming;
      }
    }
  }
}

void Router::ensureRouteMetadata(RouteNode& node, CompiledRoute&& route) {
  if (node.route == nullptr) {
    node.route = _compiledRoutePool.allocateAndConstruct(std::move(route));
    return;
  }

  const CompiledRoute& existing = *node.route;
  if (existing.paramNames != route.paramNames) {
    throw exception("Conflicting parameter naming for identical path pattern");
  }
  if (existing.hasWildcard != route.hasWildcard) {
    throw exception("Conflicting wildcard usage for identical path pattern");
  }
  node.route->hasNoSlashRegistered |= route.hasNoSlashRegistered;
  node.route->hasWithSlashRegistered |= route.hasWithSlashRegistered;
}

bool Router::matchPatternSegment(const CompiledSegment& segmentPattern, std::string_view segmentValue) {
  if (segmentPattern.type() != CompiledSegment::Type::Pattern) {
    return false;
  }

  std::size_t pos = 0;
  for (uint32_t idx = 0; idx < segmentPattern.parts.size(); ++idx) {
    const SegmentPart& part = segmentPattern.parts[idx];
    if (part.kind() == SegmentPart::Kind::Literal) {
      if (pos > segmentValue.size() || !segmentValue.substr(pos).starts_with(part.literal)) {
        return false;
      }
      pos += part.literal.size();
      continue;
    }

    const auto captureStart = pos;
    auto captureEnd = segmentValue.size();
    if (idx + 1 < segmentPattern.parts.size()) {
      const SegmentPart& next = segmentPattern.parts[idx + 1];
      if (next.kind() != SegmentPart::Kind::Literal) {
        return false;  // consecutive params not permitted
      }
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

bool Router::matchWithWildcard(const RouteNode& node, bool requestHasTrailingSlash,
                               const RouteNode*& matchedNode) const {
  if (node.wildcardChild == nullptr) {
    return false;
  }
  const RouteNode* wildcardNode = node.wildcardChild;
  if (wildcardNode->route == nullptr) {
    return false;
  }
  if (_config.trailingSlashPolicy == RouterConfig::TrailingSlashPolicy::Strict) {
    if (requestHasTrailingSlash) {
      if (!wildcardNode->route->hasWithSlashRegistered) {
        return false;
      }
    } else {
      if (!wildcardNode->route->hasNoSlashRegistered) {
        return false;
      }
    }
  }
  matchedNode = wildcardNode;
  return true;
}

bool Router::matchImpl(bool requestHasTrailingSlash, const RouteNode*& matchedNode) {
  if (_pRootRouteNode == nullptr) {
    return false;
  }

  _matchStateBuffer.clear();
  _stackBuffer.clear();

  _stackBuffer.emplace_back(_pRootRouteNode, 0, 0, 0);

  while (!_stackBuffer.empty()) {
    StackFrame frame = std::move(_stackBuffer.back());
    _stackBuffer.pop_back();

    // Terminal: all segments matched
    if (frame.segmentIndex == _segmentBuffer.size()) {
      if (frame.node->route != nullptr) {
        if (_config.trailingSlashPolicy != RouterConfig::TrailingSlashPolicy::Strict) {
          matchedNode = frame.node;
          return true;
        }
        if (requestHasTrailingSlash ? frame.node->route->hasWithSlashRegistered
                                    : frame.node->route->hasNoSlashRegistered) {
          matchedNode = frame.node;
          return true;
        }
      }
      if (matchWithWildcard(*frame.node, requestHasTrailingSlash, matchedNode)) {
        return true;
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
      _stackBuffer.push_back(frame);
      continue;
    }

    // All children exhausted, try wildcard
    _matchStateBuffer.resize(frame.matchStateSize);
    if (matchWithWildcard(*frame.node, requestHasTrailingSlash, matchedNode)) {
      return true;
    }
    // No match found, backtrack (frame already popped)
  }

  return false;
}

Router::RoutingResult Router::match(http::Method method, std::string_view path) {
  const bool pathHasTrailingSlash = ShouldNormalize(_config.trailingSlashPolicy, path);

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

    SetMatchedHandler(method, *entryPtr, result);

    // No path params for literal-only routes
    return result;
  }

  // Slow path: split segments and match patterns via trie
  splitPathSegments(path);

  const RouteNode* pMatchedNode = nullptr;
  const bool matched = matchImpl(pathHasTrailingSlash, pMatchedNode);

  if (!matched || pMatchedNode == nullptr) {
    if (_streamingHandler) {
      result.streamingHandler = &_streamingHandler;
    } else if (_handler) {
      result.requestHandler = &_handler;
    }
    return result;
  }

  const CompiledRoute* route = pMatchedNode->route;

  // Choose which handler entry to use depending on the trailing slash policy.
  const PathHandlerEntry* entryPtr =
      computePathHandlerEntry(*pMatchedNode, pathHasTrailingSlash, result.redirectPathIndicator);
  if (entryPtr == nullptr) {
    return result;
  }

  SetMatchedHandler(method, *entryPtr, result);

  const auto paramCount = std::min(_matchStateBuffer.size(), route->paramNames.size());

  _pathParamCaptureBuffer.clear();
  for (uint32_t paramPos = 0; paramPos < paramCount; ++paramPos) {
    _pathParamCaptureBuffer.emplace_back(route->paramNames[paramPos], _matchStateBuffer[paramPos]);
  }

  result.pathParams = _pathParamCaptureBuffer;

  return result;
}

http::MethodBmp Router::allowedMethods(std::string_view path) {
  const bool pathHasTrailingSlash = ShouldNormalize(_config.trailingSlashPolicy, path);

  // Fast path: O(1) lookup for literal-only routes
  if (const auto it = _literalOnlyRoutes.find(path); it != _literalOnlyRoutes.end()) {
    const RouteNode* matchedNode = it->second;
    const auto& entry = pathHasTrailingSlash ? matchedNode->handlersWithSlash : matchedNode->handlersNoSlash;
    return static_cast<http::MethodBmp>(entry.normalMethodBmp | entry.streamingMethodBmp);
  }

  // Slow path: split segments and match patterns via trie
  splitPathSegments(path);

  const RouteNode* matchedNode = nullptr;
  const bool matched = matchImpl(pathHasTrailingSlash, matchedNode);
  if (matched && matchedNode != nullptr) {
    const auto& entry = pathHasTrailingSlash ? matchedNode->handlersWithSlash : matchedNode->handlersNoSlash;
    return static_cast<http::MethodBmp>(entry.normalMethodBmp | entry.streamingMethodBmp);
  }

  if (_streamingHandler || _handler) {
    static constexpr http::MethodBmp kAllMethods = (1U << http::kNbMethods) - 1U;
    return kAllMethods;
  }
  return 0U;
}

Router::CompiledRoute Router::compilePattern(std::string_view path) {
  if (path.empty() || path.front() != '/') {
    throw exception("Router paths must begin with '/'");
  }

  CompiledRoute route;
  bool sawNamed = false;
  bool sawUnnamed = false;

  std::size_t pos = 1U;
  while (pos < path.size()) {
    const std::size_t nextSlash = path.find('/', pos);
    const std::string_view segment =
        nextSlash == std::string_view::npos ? path.substr(pos) : path.substr(pos, nextSlash - pos);

    if (segment.empty()) {
      throw exception("Router path contains empty segment");
    }

    if (segment == "*") {
      if (nextSlash != std::string_view::npos) {
        throw exception("Wildcard segment must be terminal");
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

    RawChars literalBuffer;
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
        throw exception("Unterminated '{' in router pattern");
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
        throw exception("Consecutive parameters without separator are not allowed");
      }
      previousWasParam = true;
      hasParam = true;

      const std::string_view paramName = segment.substr(i + 1U, closePos - i - 1U);
      if (paramName.empty()) {
        sawUnnamed = true;
        route.paramNames.emplace_back(std::to_string(route.paramNames.size()));
      } else {
        sawNamed = true;
        route.paramNames.emplace_back(paramName);
      }

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
    throw exception("Cannot mix named and unnamed parameters in a single path pattern");
  }

  if (!sawNamed) {
    // Ensure generated index names are stable (already generated sequentially)
    for (uint32_t i = 0; i < route.paramNames.size(); ++i) {
      route.paramNames[i] = std::to_string(i);
    }
  }

  return route;
}

const Router::PathHandlerEntry* Router::computePathHandlerEntry(
    const RouteNode& matchedNode, bool pathHasTrailingSlash,
    RoutingResult::RedirectSlashMode& redirectSlashMode) const {
  switch (_config.trailingSlashPolicy) {
    case RouterConfig::TrailingSlashPolicy::Strict:
      return pathHasTrailingSlash ? &matchedNode.handlersWithSlash : &matchedNode.handlersNoSlash;
    case RouterConfig::TrailingSlashPolicy::Normalize: {
      // Normalize accepts both variants. Prefer the variant that actually has handlers
      // for the requested form, otherwise fall back to the other variant.
      const auto& matchedSlash = pathHasTrailingSlash ? matchedNode.handlersWithSlash : matchedNode.handlersNoSlash;
      const auto& matchedNoSlash = pathHasTrailingSlash ? matchedNode.handlersNoSlash : matchedNode.handlersWithSlash;

      if ((matchedSlash.normalMethodBmp != 0U) || (matchedSlash.streamingMethodBmp != 0U)) {
        return &matchedSlash;
      }
      return &matchedNoSlash;
    }
    case RouterConfig::TrailingSlashPolicy::Redirect:
      // If only the opposite-slashed variant is registered, tell the caller to redirect.
      if (pathHasTrailingSlash) {
        if ((matchedNode.handlersWithSlash.normalMethodBmp != 0U) ||
            (matchedNode.handlersWithSlash.streamingMethodBmp != 0U)) {
          return &matchedNode.handlersWithSlash;
        }
        if ((matchedNode.handlersNoSlash.normalMethodBmp != 0U) ||
            (matchedNode.handlersNoSlash.streamingMethodBmp != 0U)) {
          redirectSlashMode = RoutingResult::RedirectSlashMode::RemoveSlash;
        }
      } else {
        if ((matchedNode.handlersNoSlash.normalMethodBmp != 0U) ||
            (matchedNode.handlersNoSlash.streamingMethodBmp != 0U)) {
          return &matchedNode.handlersNoSlash;
        }
        if ((matchedNode.handlersWithSlash.normalMethodBmp != 0U) ||
            (matchedNode.handlersWithSlash.streamingMethodBmp != 0U)) {
          redirectSlashMode = RoutingResult::RedirectSlashMode::AddSlash;
        }
      }
      return nullptr;
    default:
      std::unreachable();
  }
}

void Router::SetMatchedHandler(http::Method method, const PathHandlerEntry& entry, RoutingResult& result) {
  auto methodIdx = toMethodIdx(method);
  if (method == http::Method::HEAD) {
    static constexpr auto kHeadIdx = toMethodIdx(http::Method::HEAD);
    static constexpr auto kGetIdx = toMethodIdx(http::Method::GET);
    if (!entry.normalHandlers[kHeadIdx] && !entry.streamingHandlers[kHeadIdx]) {
      if (entry.normalHandlers[kGetIdx] || entry.streamingHandlers[kGetIdx]) {
        method = http::Method::GET;
        methodIdx = kGetIdx;
      }
    }
  }

  if (entry.streamingHandlers[methodIdx] && http::isMethodSet(entry.streamingMethodBmp, method)) {
    result.streamingHandler = &entry.streamingHandlers[methodIdx];
  } else if (entry.normalHandlers[methodIdx] && http::isMethodSet(entry.normalMethodBmp, method)) {
    result.requestHandler = &entry.normalHandlers[methodIdx];
  } else {
    result.methodNotAllowed = true;
  }
}

void Router::cloneFrom(const Router& other) {
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

  _config = other._config;
  _handler = other._handler;
  _streamingHandler = other._streamingHandler;

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
    if (nodePtr->route != nullptr) {
      if (seen.find(nodePtr->route) == seen.end()) {
        // Clone the compiled route into our pool
        CompiledRoute* cloned = _compiledRoutePool.allocateAndConstruct(*nodePtr->route);
        routeMap.emplace(nodePtr->route, cloned);
        seen.emplace(nodePtr->route, true);
      }
    }

    for (const auto& [seg, child] : nodePtr->literalChildren) {
      if (child != nullptr) {
        nodesToVisit.push_back(child);
      }
    }
    for (const auto& edge : nodePtr->dynamicChildren) {
      if (edge.child != nullptr) {
        nodesToVisit.push_back(edge.child);
      }
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
    if (src->route != nullptr) {
      if (const auto it = routeMap.find(src->route); it != routeMap.end()) {
        dst->route = it->second;
      } else {
        dst->route = nullptr;
      }
    } else {
      dst->route = nullptr;
    }

    // Clone literal children
    dst->literalChildren.clear();
    dst->literalChildren.reserve(src->literalChildren.size());
    for (const auto& [segment, childPtr] : src->literalChildren) {
      if (childPtr == nullptr) {
        continue;
      }
      RouteNode* newChild = _nodePool.allocateAndConstruct();
      dst->literalChildren.emplace(segment, newChild);
      workQueue.emplace_back(childPtr, newChild);
    }

    // Clone dynamic children
    dst->dynamicChildren.clear();
    dst->dynamicChildren.reserve(src->dynamicChildren.size());
    for (const auto& edge : src->dynamicChildren) {
      if (edge.child == nullptr) {
        continue;
      }
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
    // Look up the cloned node in our mapping
    if (const auto it = nodeMap.find(oldNode); it != nodeMap.end()) {
      _literalOnlyRoutes.emplace(path, it->second);
    }
  }
}

}  // namespace aeronet
