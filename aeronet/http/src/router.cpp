#include "aeronet/router.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <ranges>
#include <span>
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

void UnescapePattern(std::string_view input, RawChars32& output) {
  output.assign(input);

  // Unescape any reserved characters by removing the duplicate
  // For path = "/api/{{version}}/data", we want to store "/api/{version}/data"
  static_assert(kEscapedOpenBrace.size() == 2U && kEscapedCloseBrace.size() == 2U);
  for (std::string_view escaped : {kEscapedOpenBrace, kEscapedCloseBrace}) {
    for (auto it = output.begin(); it != output.end(); ++it) {
      it = std::search(it, output.end(), escaped.begin(), escaped.end());
      if (it == output.end()) {
        break;
      }
      std::memmove(it, it + 1U, static_cast<std::size_t>(output.end() - (it + 1U)));
      output.setSize(output.size() - 1U);
    }
  }
}

bool MayNormalizeHasTrailingSlash(RouterConfig::TrailingSlashPolicy policy, std::string_view& path) {
  const auto sz = path.size();
  if (sz == 0) {
    throw std::invalid_argument("Path cannot be empty");
  }
  const bool pathHasTrailingSlash = sz > 1U && path.back() == '/';
  if (policy == RouterConfig::TrailingSlashPolicy::Strict) {
    return pathHasTrailingSlash;
  }
  path.remove_suffix(static_cast<std::string_view::size_type>(pathHasTrailingSlash));
  return pathHasTrailingSlash;
}

void indent(std::ostream& os, int depth) {
  for (int i = 0; i < depth; ++i) {
    os << "│   ";
  }
}

}  // namespace

Router::Router(const Router& other)
    : _config(other._config),
      _handler(other._handler),
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
      _asyncHandler(other._asyncHandler),
#endif
      _streamingHandler(other._streamingHandler),
      _globalPreMiddleware(other._globalPreMiddleware),
      _globalPostMiddleware(other._globalPostMiddleware) {
  cloneNodesFrom(other);
}

Router& Router::operator=(const Router& other) {
  if (this != &other) [[likely]] {
    _config = other._config;
    _handler = other._handler;
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    _asyncHandler = other._asyncHandler;
#endif
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

void Router::setDefault(RequestHandler handler) {
  _handler = std::move(handler);
  if (_streamingHandler) {
    log::warn("Overwriting existing default streaming handler with normal handler");
    _streamingHandler = {};
  }
}

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

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
PathHandlerEntry& Router::setPath(http::MethodBmp methods, std::string_view path, AsyncRequestHandler handler) {
  return setPathInternal(methods, path, std::move(handler));
}

PathHandlerEntry& Router::setPath(http::Method method, std::string_view path, AsyncRequestHandler handler) {
  return setPathInternal(static_cast<http::MethodBmp>(method), path, std::move(handler));
}
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
PathHandlerEntry& Router::setWebSocket(std::string_view path, WebSocketEndpoint endpoint) {
  return setPathInternal({}, path, std::move(endpoint));
}
#endif

// ============================================================================
// Radix Tree Helper Functions
// ============================================================================

namespace {

std::size_t LongestCommonPrefix(std::string_view pathA, std::string_view pathB) noexcept {
  const std::size_t maxLen = std::min(pathA.size(), pathB.size());
  std::size_t idx = 0;
  while (idx < maxLen && pathA[idx] == pathB[idx]) {
    ++idx;
  }
  return idx;
}

// Find the position of a wildcard (:param or {param} or *) in the path
// Returns {start_position, end_position} of the wildcard, or npos if none found
struct WildcardInfo {
  std::size_t first{std::string_view::npos};
  std::size_t last{std::string_view::npos};
};

std::size_t FindUnescaped(std::string_view sv, char ch, std::size_t pos = 0) noexcept {
  for (pos = sv.find(ch, pos); pos != std::string_view::npos;) {
    if (pos + 1 < sv.size() && sv[pos + 1] == ch) {
      pos = sv.find(ch, pos + 2U);
      continue;
    }
    break;
  }
  return pos;
}

// Find start of wildcard ('{' for param or '*' for catch-all)
WildcardInfo FindWildcard(std::string_view path) noexcept {
  const auto maxPos = path.size() - 1U;
  for (std::size_t pos = 0; pos < maxPos; ++pos) {
    const char ch = path[pos];
    if (ch != '{') {
      continue;
    }
    // Handle escaped braces
    if (path[pos + 1] == '{') {
      ++pos;
      continue;
    }

    // The wildcard includes everything from '{' to the end of the segment
    // For the radix tree, we treat the entire segment containing params specially
    std::size_t segEnd = path.find('/', pos + 1U);
    if (segEnd == std::string_view::npos) {
      segEnd = path.size();
    }
    return {pos, segEnd};
  }
  if (path.ends_with("/*")) {
    return {path.size() - 1U, path.size()};
  }
  return {};
}

}  // namespace

Router::ParsedRoute Router::ParsePattern(std::string_view path) {
  assert(!path.empty());
  if (path.front() != '/') {
    throw std::invalid_argument("Router paths must begin with '/'");
  }

  ParsedRoute route;
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
        // * alone in a non-terminal segment: treat as literal, not a wildcard.
        // This is consistent with the new behavior where * is a wildcard only if
        // it's the sole character of a terminal segment.
        // Continue processing as a normal literal segment.
      } else {
        // * alone in a terminal segment: this is a wildcard
        route.hasWildcard = true;
        break;
      }
    }

    // Check for params in this segment
    bool previousWasParam = false;
    for (std::size_t idx = 0; idx < segment.size();) {
      // Handle escaped braces
      if (segment.compare(idx, kEscapedOpenBrace.size(), kEscapedOpenBrace) == 0) {
        idx += kEscapedOpenBrace.size();
        previousWasParam = false;
        continue;
      }
      if (segment.compare(idx, kEscapedCloseBrace.size(), kEscapedCloseBrace) == 0) {
        idx += kEscapedCloseBrace.size();
        continue;
      }

      if (segment[idx] != '{') {
        ++idx;
        previousWasParam = false;
        continue;
      }

      auto closePos = FindUnescaped(segment, '}', idx + 1U);

      if (closePos == std::string_view::npos) {
        throw std::invalid_argument("Unterminated '{' in router pattern");
      }

      if (previousWasParam) {
        throw std::invalid_argument("Consecutive parameters without separator are not allowed");
      }
      previousWasParam = true;

      const std::string_view paramName = segment.substr(idx + 1U, closePos - idx - 1U);
      if (paramName.empty()) {
        sawUnnamed = true;
        route.paramNames.append(std::string_view(IntegralToCharVector(paramIdx)));
      } else {
        sawNamed = true;
        route.paramNames.append(paramName);
      }
      ++paramIdx;
      idx = closePos + 1U;
    }

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

// Increment child priority and reorder if needed, returns new position
std::size_t Router::RadixNode::incrementChildPrio(std::size_t pos) {
  assert(pos < indices.size());

  children[static_cast<uint32_t>(pos)]->priority++;
  const uint32_t prio = children[static_cast<uint32_t>(pos)]->priority;

  // Move to front while priority is higher than predecessor
  std::size_t newPos = pos;
  while (newPos > 0 && children[static_cast<uint32_t>(newPos - 1)]->priority < prio) {
    std::swap(children[static_cast<uint32_t>(newPos - 1)], children[static_cast<uint32_t>(newPos)]);
    --newPos;
  }

  // Update indices string if position changed
  if (newPos < pos) {
    // Move element at pos to newPos (shift right)
    std::rotate(indices.begin() + newPos, indices.begin() + pos, indices.begin() + pos + 1);
  }

  return newPos;
}

void Router::insertChild(RadixNode& node, std::string_view path, [[maybe_unused]] const ParsedRoute& route) {
  RadixNode* pNode = &node;

  while (true) {
    // Find position of first wildcard
    auto [wildcardFirst, wildcardLast] = FindWildcard(path);

    if (wildcardFirst == std::string_view::npos) {
      // No wildcard found, simply store the path
      UnescapePattern(path, pNode->path);
      return;
    }

    // Wildcard conflict should have been caught earlier
    // Already checked by throw std::invalid_argument("Path conflicts with existing wildcard route");
    assert(!pNode->hasWildChild);

    // Handle param: {name} or {name}literal or prefix{name}
    std::string_view wildcard(path.data() + wildcardFirst, path.data() + wildcardLast);
    assert(!wildcard.empty());
    if (wildcard[0] == '{') {
      if (wildcardFirst > 0) {
        // Insert prefix before the wildcard
        UnescapePattern(path.substr(0, wildcardFirst), pNode->path);
        path = path.substr(wildcardFirst);
      }

      // Find the end of the segment containing the param
      std::size_t segEnd = path.find('/');
      std::string_view paramSegment = segEnd == std::string_view::npos ? path : path.substr(0, segEnd);

      // Parse the param segment into parts
      pNode->hasWildChild = true;
      RadixNode* pChild = _nodePool.allocateAndConstruct();
      pChild->nodeType = NodeType::Param;
      UnescapePattern(paramSegment, pChild->path);

      // Parse the segment parts (literal/param alternation)
      for (std::size_t idx = 0; idx < paramSegment.size();) {
        // Handle escaped braces
        if (paramSegment.compare(idx, kEscapedOpenBrace.size(), kEscapedOpenBrace) == 0) {
          pChild->paramParts.emplace_back().literal.push_back('{');
          idx += kEscapedOpenBrace.size();
          continue;
        }
        if (paramSegment.compare(idx, kEscapedCloseBrace.size(), kEscapedCloseBrace) == 0) {
          if (!pChild->paramParts.empty() && pChild->paramParts.back().kind() == SegmentPart::Kind::Literal) {
            pChild->paramParts.back().literal.push_back('}');
          } else {
            pChild->paramParts.emplace_back().literal.push_back('}');
          }
          idx += kEscapedCloseBrace.size();
          continue;
        }

        if (paramSegment[idx] != '{') {
          // Literal character
          if (pChild->paramParts.empty() || pChild->paramParts.back().kind() != SegmentPart::Kind::Literal) {
            pChild->paramParts.emplace_back();
          }
          pChild->paramParts.back().literal.push_back(paramSegment[idx]);
          ++idx;
          continue;
        }

        // Parameter
        const std::size_t closePos = paramSegment.find('}', idx + 1);
        assert(closePos != std::string_view::npos);  // Already validated

        // should have been caught by ParsePattern
        pChild->paramParts.emplace_back();  // Empty literal = param placeholder
        idx = closePos + 1U;
      }

      pNode->children.push_back(pChild);
      pNode = pChild;
      ++pNode->priority;

      // If path doesn't end with the param segment, continue with remaining path
      if (segEnd != std::string_view::npos && segEnd < path.size()) {
        path = path.substr(segEnd);
        RadixNode* pNextChild = _nodePool.allocateAndConstruct();
        pNextChild->priority = 1;
        pNode->children.push_back(pNextChild);
        pNode->indices.push_back('/');
        pNode = pNextChild;
        continue;
      }
      return;
    }

    // Store path up to (but not including) the '/*'
    if (wildcardFirst > 0) {
      UnescapePattern(path.substr(0, wildcardFirst - 1), pNode->path);  // Exclude the trailing '/'
    }

    // Create catch-all node
    // Note: We don't add catch-all to indices - it's accessed via hasWildChild
    pNode->hasWildChild = true;
    RadixNode* pCatchAllNode = _nodePool.allocateAndConstruct();
    pCatchAllNode->nodeType = NodeType::CatchAll;
    pCatchAllNode->path.assign("/*");
    pNode->children.push_back(pCatchAllNode);
    // Do NOT add to indices: currentNode->indices.push_back('/');

    return;
  }
}

Router::RadixNode* Router::insertRoute(std::string_view path, ParsedRoute&& route, bool pathHasTrailingSlash) {
  if (_pRootNode == nullptr) {
    _pRootNode = _nodePool.allocateAndConstruct();
  }

  RadixNode* pNode = _pRootNode;
  ++pNode->priority;

  // Empty tree case
  if (pNode->path.empty() && pNode->indices.empty()) {
    insertChild(*pNode, path, route);

    // Find the terminal node
    while (!pNode->children.empty()) {
      pNode = pNode->children[0];
    }

    // Store route metadata
    pNode->pRoute = _compiledRoutePool.allocateAndConstruct();
    pNode->pRoute->paramNames = std::move(route.paramNames);
    pNode->pRoute->hasWildcard = route.hasWildcard;
    if (pathHasTrailingSlash) {
      pNode->pRoute->hasWithSlashRegistered = true;
    } else {
      pNode->pRoute->hasNoSlashRegistered = true;
    }
    return pNode;
  }

  // Walk the tree
  while (true) {
    // Find the longest common prefix
    const std::string_view currentPath = pNode->path;
    const std::size_t commonPrefixLen = LongestCommonPrefix(path, currentPath);

    // Split edge if common prefix is shorter than current node's path
    if (commonPrefixLen < currentPath.size()) {
      // Create a child node with the remainder of the current node's path
      RadixNode* pChild = _nodePool.allocateAndConstruct();
      pChild->path.assign(currentPath.substr(commonPrefixLen));
      pChild->hasWildChild = pNode->hasWildChild;
      pChild->indices = std::move(pNode->indices);
      pChild->children = std::move(pNode->children);
      pChild->handlers = std::move(pNode->handlers);
      pChild->pRoute = pNode->pRoute;
      pChild->priority = pNode->priority - 1;
      pChild->paramParts = std::move(pNode->paramParts);

      // Current node becomes the common prefix
      pNode->children = {pChild};
      pNode->indices.clear();
      pNode->indices.push_back(currentPath[commonPrefixLen]);
      pNode->path.assign(currentPath.substr(0, commonPrefixLen));
      pNode->hasWildChild = false;
      pNode->handlers = {};
      pNode->pRoute = nullptr;
      pNode->paramParts.clear();
    }

    // Make new node a child of this node
    if (commonPrefixLen < path.size()) {
      path = path.substr(commonPrefixLen);
      const char firstChar = path[0];

      // First, check if a static child with the next path byte exists
      const std::string_view indices = pNode->indices;
      const auto foundPos = indices.find(firstChar);
      if (foundPos != std::string_view::npos) {
        pNode = pNode->children[static_cast<uint32_t>(pNode->incrementChildPrio(foundPos))];
        continue;
      }

      // No static match found. If we have a wildcard child and the path starts with a wildcard,
      // try to use it. Also check for catch-all pattern "/*"
      if (pNode->hasWildChild && (firstChar == '{' || path == "/*")) {
        // Wildcard child is always at the end of children array (after static children)
        // Static children count = indices.size()
        const auto wildcardIdx = indices.size();
        if (wildcardIdx < pNode->children.size()) {
          RadixNode* pWildcardChild = pNode->children[static_cast<uint32_t>(wildcardIdx)];

          if (pWildcardChild->nodeType != NodeType::Static) {
            pNode = pWildcardChild;
            ++pNode->priority;

            // Check if the wildcard matches
            const std::string_view nodePath = pNode->path;

            // Special case for catch-all: if both are catch-all, allow re-registration
            if (pNode->nodeType == NodeType::CatchAll) {
              // The remaining path should be either "*" or "/*"
              if (path == "*" || path == "/*") {
                // This is the same catch-all pattern, allow it
                assert(pNode->pRoute != nullptr);
                assert(pNode->pRoute->hasWildcard);
                if (pathHasTrailingSlash) {
                  pNode->pRoute->hasWithSlashRegistered = true;
                } else {
                  pNode->pRoute->hasNoSlashRegistered = true;
                }
                return pNode;
              }
              throw std::invalid_argument("Path conflicts with existing wildcard route");
            }

            if (path.size() >= nodePath.size() && path.starts_with(nodePath) &&
                (nodePath.size() >= path.size() || path[nodePath.size()] == '/')) {
              continue;
            }

            // Wildcard conflict
            throw std::invalid_argument("Path conflicts with existing wildcard route");
          }
        }
      }

      // - Param nodes are created in insertChild() like this:
      //   pNode->children.push_back(child);
      //   where child is the next segment, not '/'.
      // - A param node only ever has:
      //   * zero children
      //   * or a static child whose path starts with '/', but never an index '/'
      // - indices is only populated for static children, never for param edges.
      // -> below condition is always true, unreachable param '/' child
      assert(pNode->nodeType != NodeType::Param || firstChar != '/' || pNode->children.size() != 1);

      // No matching child found, insert new one
      // Don't create a static child if the path is a catch-all pattern
      if (firstChar != '{' && path != "/*") {
        pNode->indices.push_back(firstChar);
        RadixNode* pChild = _nodePool.allocateAndConstruct();
        // If there's a wildcard child, it must stay at the end.
        // Insert static child at position indices.size() - 1 (just added index)
        if (pNode->hasWildChild && !pNode->children.empty()) {
          // Insert before the wildcard child (which is at the end)
          pNode->children.insert(pNode->children.end() - 1, pChild);
        } else {
          pNode->children.push_back(pChild);
        }
        pNode->incrementChildPrio(pNode->indices.size() - 1);
        pNode = pChild;
      }

      insertChild(*pNode, path, route);

      // Find the terminal node
      while (!pNode->children.empty() && pNode->pRoute == nullptr) {
        pNode = pNode->children[0];
      }

      // Store route metadata
      if (pNode->pRoute == nullptr) {
        pNode->pRoute = _compiledRoutePool.allocateAndConstruct();
        pNode->pRoute->paramNames = std::move(route.paramNames);
        pNode->pRoute->hasWildcard = route.hasWildcard;
      }

      if (pathHasTrailingSlash) {
        pNode->pRoute->hasWithSlashRegistered = true;
      } else {
        pNode->pRoute->hasNoSlashRegistered = true;
      }
      return pNode;
    }

    // We've reached the end of the path, this node should have the handler
    if (pNode->pRoute == nullptr) {
      pNode->pRoute = _compiledRoutePool.allocateAndConstruct();
      pNode->pRoute->paramNames = std::move(route.paramNames);
      pNode->pRoute->hasWildcard = route.hasWildcard;
    }

    if (pathHasTrailingSlash) {
      pNode->pRoute->hasWithSlashRegistered = true;
    } else {
      pNode->pRoute->hasNoSlashRegistered = true;
    }
    return pNode;
  }
}

PathHandlerEntry& Router::setPathInternal(http::MethodBmp methods, std::string_view path,
                                          HandlerVariant handlerVariant) {
  const bool pathHasTrailingSlash = MayNormalizeHasTrailingSlash(_config.trailingSlashPolicy, path);

  ParsedRoute parsed = ParsePattern(path);
  const bool isLiteralOnly = parsed.paramNames.empty() && !parsed.hasWildcard;
  if (pathHasTrailingSlash) {
    parsed.hasWithSlashRegistered = true;
  } else {
    parsed.hasNoSlashRegistered = true;
  }

  PathHandlerEntry* pEntry;
  if (isLiteralOnly) {
    RawChars32 unescapedPath;
    UnescapePattern(path, unescapedPath);
    LiteralRouteEntry& literalEntry = _literalOnlyRoutes[std::move(unescapedPath)];

    if (pathHasTrailingSlash) {
      literalEntry.hasWithSlashRegistered = true;
    } else {
      literalEntry.hasNoSlashRegistered = true;
    }

    pEntry = &literalEntry.handlers;
  } else {
    RadixNode* pNode = insertRoute(path, std::move(parsed), pathHasTrailingSlash);
    pEntry = &pNode->handlers;
  }

  std::visit(
      [pEntry, methods, path](auto&& handler) {
        using T = std::decay_t<decltype(handler)>;
        if constexpr (std::is_same_v<T, RequestHandler>) {
          if (!handler) {
            throw std::invalid_argument("Cannot set empty RequestHandler");
          }
          if ((pEntry->_normalMethodBmp & methods) != 0) {
            log::warn("Overwriting existing path handler for {}", path);
          }
          pEntry->assignNormalHandler(methods, std::forward<decltype(handler)>(handler));
        } else if constexpr (std::is_same_v<T, StreamingHandler>) {
          if (!handler) {
            throw std::invalid_argument("Cannot set empty StreamingHandler");
          }
          if ((pEntry->_streamingMethodBmp & methods) != 0) {
            log::warn("Overwriting existing streaming path handler for {}", path);
          }
          pEntry->assignStreamingHandler(methods, std::forward<decltype(handler)>(handler));
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
        } else if constexpr (std::is_same_v<T, AsyncRequestHandler>) {
          if (!handler) {
            throw std::invalid_argument("Cannot set empty AsyncRequestHandler");
          }
          if ((pEntry->_asyncMethodBmp & methods) != 0) {
            log::warn("Overwriting existing async path handler for {}", path);
          }
          pEntry->assignAsyncHandler(methods, std::forward<decltype(handler)>(handler));
#endif
#ifdef AERONET_ENABLE_WEBSOCKET
        } else if constexpr (std::is_same_v<T, WebSocketEndpoint>) {
          pEntry->assignWebSocketEndpoint(std::forward<decltype(handler)>(handler));
#endif
        } else {
          static_assert(false, "Non-exhaustive visitor!");
        }
      },
      std::move(handlerVariant));

  return *pEntry;
}

// ============================================================================
// Matching Implementation
// ============================================================================

bool Router::matchParamParts(std::span<const SegmentPart> parts, std::string_view segment) {
  std::size_t pos = 0;

  for (uint32_t idx = 0; idx < parts.size(); ++idx) {
    const SegmentPart& part = parts[idx];

    if (part.kind() == SegmentPart::Kind::Literal) {
      assert(pos <= segment.size());
      if (!segment.substr(pos).starts_with(part.literal)) {
        return false;
      }
      pos += part.literal.size();
      continue;
    }

    // Parameter capture
    const auto captureStart = pos;
    auto captureEnd = segment.size();

    if (idx + 1 < parts.size()) {
      const SegmentPart& next = parts[idx + 1];
      assert(next.kind() == SegmentPart::Kind::Literal);
      const std::size_t found = segment.find(next.literal, pos);
      if (found == std::string_view::npos) {
        return false;
      }
      captureEnd = found;
      pos = found;
    } else {
      pos = segment.size();
    }

    _matchStateBuffer.push_back(segment.substr(captureStart, captureEnd - captureStart));
  }

  return pos == segment.size();
}

const Router::RadixNode* Router::matchImpl(std::string_view path, bool requestHasTrailingSlash) {
  if (_pRootNode == nullptr) {
    return nullptr;
  }

  _matchStateBuffer.clear();
  const RadixNode* pNode = _pRootNode;

  // Walk the tree
  while (true) {
    const std::string_view prefix = pNode->path;

    if (path.size() > prefix.size()) {
      if (path.starts_with(prefix)) {
        path = path.substr(prefix.size());
        const char firstChar = path[0];

        // First, try to match a static child by finding one whose path is a prefix of remaining path
        const std::string_view indices = pNode->indices;
        for (std::size_t idx = 0; idx < indices.size(); ++idx) {
          if (indices[idx] == firstChar) {
            const RadixNode* pChild = pNode->children[static_cast<uint32_t>(idx)];
            const std::string_view childPath = pChild->path;
            // Verify this child's path is actually a prefix of remaining path
            if (path.size() >= childPath.size() && path.starts_with(childPath)) {
              pNode = pChild;
              goto continueWalk;
            }
            // Index matched but prefix didn't - fall through to try wildcard
          }
        }

        // No static match. Try wildcard child if present
        if (pNode->hasWildChild) {
          // Wildcard child is always at the end of children array (after static children)
          const auto wildcardIdx = pNode->indices.size();
          if (wildcardIdx < pNode->children.size()) {
            const RadixNode* pWildcardChild = pNode->children[static_cast<uint32_t>(wildcardIdx)];

            if (pWildcardChild->nodeType != NodeType::Static) {
              pNode = pWildcardChild;

              assert(pNode->nodeType == NodeType::Param || pNode->nodeType == NodeType::CatchAll);

              if (pNode->nodeType == NodeType::Param) {
                // Find param end (either '/' or path end)
                std::size_t segEnd = path.find('/');
                const std::string_view segment = path.substr(0, segEnd);

                // Match param pattern if we have parts
                if (!pNode->paramParts.empty()) {
                  if (!matchParamParts(pNode->paramParts, segment)) {
                    return nullptr;
                  }
                } else {
                  // Simple param - capture the whole segment
                  _matchStateBuffer.push_back(segment);
                }

                // Continue to next segment if there's more path
                if (segEnd < path.size()) {
                  if (!pNode->children.empty()) {
                    path = path.substr(segEnd);
                    pNode = pNode->children[0];
                    goto continueWalk;
                  }
                  // More path but no children
                  return nullptr;
                }

                // End of path
                if (pNode->pRoute != nullptr) {
                  if (_config.trailingSlashPolicy != RouterConfig::TrailingSlashPolicy::Strict) {
                    return pNode;
                  }
                  if (requestHasTrailingSlash ? pNode->pRoute->hasWithSlashRegistered
                                              : pNode->pRoute->hasNoSlashRegistered) {
                    return pNode;
                  }
                }

                // Check for trailing slash redirect (TSR) - only in non-strict mode
                if (_config.trailingSlashPolicy != RouterConfig::TrailingSlashPolicy::Strict &&
                    pNode->children.size() == 1) {
                  const RadixNode* pChild = pNode->children[0];
                  if (std::string_view(pChild->path) == "/" && pChild->pRoute != nullptr) {
                    return pNode;  // TSR case
                  }
                }
                return nullptr;
              }

              // Catch-all matches everything remaining
              // Wildcards don't produce captures in the current design
              if (pNode->pRoute != nullptr) {
                if (_config.trailingSlashPolicy != RouterConfig::TrailingSlashPolicy::Strict) {
                  return pNode;
                }
                // Catch-all with trailing slash in request
                if (requestHasTrailingSlash) {
                  return nullptr;  // catch-all registered without slash by design
                }
                if (pNode->pRoute->hasNoSlashRegistered) {
                  return pNode;
                }
              }
              return nullptr;
            }
          }
        }

        // No matching child found — this path segment does not match any registered route.
        // Return nullptr so the caller can fall back to the default handler (e.g. static files).
        return nullptr;
      }
    } else if (path == prefix) {
      // Exact match - we should have reached the node containing the handler
      if (pNode->pRoute != nullptr) {
        if (_config.trailingSlashPolicy != RouterConfig::TrailingSlashPolicy::Strict) {
          return pNode;
        }
        if (requestHasTrailingSlash ? pNode->pRoute->hasWithSlashRegistered : pNode->pRoute->hasNoSlashRegistered) {
          return pNode;
        }
      }

      // If there's a wildcard child, check for catch-all
      if (pNode->hasWildChild) {
        // Wildcard child is always at index indices.size() (after static children)
        const auto wildcardIdx = pNode->indices.size();
        if (wildcardIdx < pNode->children.size()) {
          const RadixNode* pChild = pNode->children[static_cast<uint32_t>(wildcardIdx)];
          if (pChild->nodeType == NodeType::CatchAll && pChild->pRoute != nullptr) {
            if (_config.trailingSlashPolicy != RouterConfig::TrailingSlashPolicy::Strict || !requestHasTrailingSlash) {
              return pChild;
            }
          }
        }
      }
    }

    // Nothing found
    // Check for trailing slash redirect (TSR) - only in non-strict mode
    // This handles the case where node has path "/foo" and we request "/foo/" (prefix matches with trailing /)
    if (_config.trailingSlashPolicy != RouterConfig::TrailingSlashPolicy::Strict) {
      if (prefix.size() == path.size() + 1 && prefix[path.size()] == '/' && prefix.starts_with(path) &&
          pNode->pRoute != nullptr) {
        return pNode;  // TSR case
      }
    }

    return nullptr;

  continueWalk:;
  }
}

Router::RoutingResult Router::match(http::Method method, std::string_view path) {
  const bool pathHasTrailingSlash = MayNormalizeHasTrailingSlash(_config.trailingSlashPolicy, path);

  RoutingResult result;

  if (const auto it = _literalOnlyRoutes.find(path); it != _literalOnlyRoutes.end()) {
    const auto& literalEntry = it->second;
    const PathHandlerEntry* entryPtr = nullptr;
    switch (_config.trailingSlashPolicy) {
      case RouterConfig::TrailingSlashPolicy::Strict:
        if (pathHasTrailingSlash ? literalEntry.hasWithSlashRegistered : literalEntry.hasNoSlashRegistered) {
          entryPtr = &literalEntry.handlers;
        }
        break;
      case RouterConfig::TrailingSlashPolicy::Normalize:
        if (literalEntry.handlers.hasAnyHandler()) {
          entryPtr = &literalEntry.handlers;
        }
        break;
      case RouterConfig::TrailingSlashPolicy::Redirect:
        if (pathHasTrailingSlash) {
          if (literalEntry.hasWithSlashRegistered) {
            entryPtr = &literalEntry.handlers;
          } else if (literalEntry.hasNoSlashRegistered) {
            result.redirectPathIndicator = RoutingResult::RedirectSlashMode::RemoveSlash;
          }
        } else {
          if (literalEntry.hasNoSlashRegistered) {
            entryPtr = &literalEntry.handlers;
          } else if (literalEntry.hasWithSlashRegistered) {
            result.redirectPathIndicator = RoutingResult::RedirectSlashMode::AddSlash;
          }
        }
        break;
    }

    if (entryPtr != nullptr) {
      setMatchedHandler(method, *entryPtr, result);
    }
    return result;
  }

  // Radix tree traversal
  const RadixNode* pMatchedNode = matchImpl(path, pathHasTrailingSlash);

  if (pMatchedNode == nullptr) {
    // Fall back to default handlers
    if (_streamingHandler) {
      result.setStreamingHandler(&_streamingHandler);
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    } else if (_asyncHandler) {
      result.setAsyncRequestHandler(&_asyncHandler);
#endif
    } else if (_handler) {
      result.setRequestHandler(&_handler);
    }
    return result;
  }

  const PathHandlerEntry* entryPtr =
      computePathHandlerEntry(*pMatchedNode, pathHasTrailingSlash, result.redirectPathIndicator);
  if (entryPtr == nullptr) {
    return result;
  }

  setMatchedHandler(method, *entryPtr, result);

  // Build path params from match state
  const CompiledRoute* pRoute = pMatchedNode->pRoute;
  if (pRoute != nullptr) {
    assert(std::cmp_equal(pRoute->paramNames.nbConcatenatedStrings(), _matchStateBuffer.size()));

    _pathParamCaptureBuffer.clear();
    for (auto [paramPos, param] : std::views::enumerate(pRoute->paramNames)) {
      _pathParamCaptureBuffer.emplace_back(param, _matchStateBuffer[static_cast<uint32_t>(paramPos)]);
    }
    result.pathParams = _pathParamCaptureBuffer;
  }

  return result;
}

http::MethodBmp Router::allowedMethods(std::string_view path) {
  const bool pathHasTrailingSlash = MayNormalizeHasTrailingSlash(_config.trailingSlashPolicy, path);

  if (const auto it = _literalOnlyRoutes.find(path); it != _literalOnlyRoutes.end()) {
    const auto& literalEntry = it->second;
    if (_config.trailingSlashPolicy == RouterConfig::TrailingSlashPolicy::Strict ||
        _config.trailingSlashPolicy == RouterConfig::TrailingSlashPolicy::Redirect) {
      if (pathHasTrailingSlash ? !literalEntry.hasWithSlashRegistered : !literalEntry.hasNoSlashRegistered) {
        return 0U;
      }
    }

    const auto& entry = literalEntry.handlers;
    return static_cast<http::MethodBmp>(entry._normalMethodBmp | entry._streamingMethodBmp
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
                                        | entry._asyncMethodBmp
#endif
    );
  }

  // Radix tree traversal
  const RadixNode* pMatchedNode = matchImpl(path, pathHasTrailingSlash);

  if (pMatchedNode != nullptr) {
    const auto& entry = pMatchedNode->handlers;
    return static_cast<http::MethodBmp>(entry._normalMethodBmp | entry._streamingMethodBmp
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
                                        | entry._asyncMethodBmp
#endif
    );
  }

  if (_streamingHandler || _handler
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
      || _asyncHandler
#endif
  ) {
    static constexpr http::MethodBmp kAllMethods = (1U << http::kNbMethods) - 1U;
    return kAllMethods;
  }
  return 0U;
}

const PathHandlerEntry* Router::computePathHandlerEntry(const RadixNode& matchedNode, bool pathHasTrailingSlash,
                                                        RoutingResult::RedirectSlashMode& redirectSlashMode) const {
  // We have a single handlers entry per node.
  // The CompiledRoute tracks which slash variants (with/without) were registered.
  const CompiledRoute* pRoute = matchedNode.pRoute;

  switch (_config.trailingSlashPolicy) {
    case RouterConfig::TrailingSlashPolicy::Strict:
      // In Strict mode, only return handlers if the exact slash variant was registered.
      // Since paths aren't normalized in Strict mode, different variants are in different nodes.
      // We just check if the requested variant was registered on this node.
      if (pRoute != nullptr) {
        if (pathHasTrailingSlash ? pRoute->hasWithSlashRegistered : pRoute->hasNoSlashRegistered) {
          return &matchedNode.handlers;
        }
      }
      return nullptr;

    case RouterConfig::TrailingSlashPolicy::Normalize:
      // In Normalize mode, both slash variants resolve to the same node.
      // Always return handlers if present.
      if (matchedNode.handlers.hasAnyHandler()) {
        return &matchedNode.handlers;
      }
      return nullptr;

    case RouterConfig::TrailingSlashPolicy::Redirect:
      // In Redirect mode, return handlers if the requested variant was registered.
      // Otherwise, signal a redirect to the registered variant.
      if (pRoute != nullptr) {
        if (pathHasTrailingSlash) {
          if (pRoute->hasWithSlashRegistered) {
            return &matchedNode.handlers;
          }
          if (pRoute->hasNoSlashRegistered) {
            redirectSlashMode = RoutingResult::RedirectSlashMode::RemoveSlash;
          }
        } else {
          if (pRoute->hasNoSlashRegistered) {
            return &matchedNode.handlers;
          }
          if (pRoute->hasWithSlashRegistered) {
            redirectSlashMode = RoutingResult::RedirectSlashMode::AddSlash;
          }
        }
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
    if (!entry.hasNormalHandler(kHeadIdx) && !entry.hasStreamingHandler(kHeadIdx)
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
        && !entry.hasAsyncHandler(kHeadIdx)
#endif
    ) {
      if (entry.hasNormalHandler(kGetIdx) || entry.hasStreamingHandler(kGetIdx)
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
          || entry.hasAsyncHandler(kGetIdx)
#endif
      ) {
        method = http::Method::GET;
        methodIdx = kGetIdx;
      }
    }
  }

  if (entry.hasStreamingHandler(methodIdx)) {
    assert(http::IsMethodSet(entry._streamingMethodBmp, method));
    result.setStreamingHandler(entry.streamingHandlerPtr(methodIdx));
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  } else if (entry.hasAsyncHandler(methodIdx)) {
    assert(http::IsMethodSet(entry._asyncMethodBmp, method));
    result.setAsyncRequestHandler(entry.asyncHandlerPtr(methodIdx));
#endif
  } else if (entry.hasNormalHandler(methodIdx)) {
    assert(http::IsMethodSet(entry._normalMethodBmp, method));
    result.setRequestHandler(entry.requestHandlerPtr(methodIdx));
#ifdef AERONET_ENABLE_WEBSOCKET
  } else if (entry.hasWebSocketEndpoint() && method == http::Method::GET) {
// WebSocket endpoint on GET - handler will be determined by upgrade validation
#endif
  } else {
    result.methodNotAllowed = true;
  }

  if (entry._corsPolicy.active()) {
    result.pCorsPolicy = &entry._corsPolicy;
  } else if (_config.defaultCorsPolicy.active()) {
    result.pCorsPolicy = &_config.defaultCorsPolicy;
  }

#ifdef AERONET_ENABLE_WEBSOCKET
  if (entry.hasWebSocketEndpoint()) {
    result.pWebSocketEndpoint = entry.webSocketEndpointPtr();
  }
#endif

  result.requestMiddlewareRange = entry._preMiddleware;
  result.responseMiddlewareRange = entry._postMiddleware;
  result.pathConfig = entry._pathConfig;
}

// ============================================================================
// Clone and Clear
// ============================================================================

void Router::cloneNodesFrom(const Router& other) {
  _nodePool.clear();
  _compiledRoutePool.clear();

  flat_hash_map<const CompiledRoute*, CompiledRoute*> routeMap;
  flat_hash_map<const RadixNode*, RadixNode*> nodeMap;

  // Clone strategy using BFS
  vector<std::pair<const RadixNode*, RadixNode*>> workQueue;

  if (other._pRootNode != nullptr) {
    _pRootNode = _nodePool.allocateAndConstruct();
    workQueue.emplace_back(other._pRootNode, _pRootNode);
  } else {
    _pRootNode = nullptr;
  }

  while (!workQueue.empty()) {
    auto [src, dst] = workQueue.back();
    workQueue.pop_back();

    nodeMap.emplace(src, dst);

    // Copy basic fields
    dst->path = src->path;
    dst->indices = src->indices;
    dst->priority = src->priority;
    dst->nodeType = src->nodeType;
    dst->hasWildChild = src->hasWildChild;
    dst->paramParts = src->paramParts;
    dst->handlers = src->handlers;

    // Clone or reuse compiled route
    if (src->pRoute != nullptr) {
      auto it = routeMap.find(src->pRoute);
      if (it == routeMap.end()) {
        CompiledRoute* cloned = _compiledRoutePool.allocateAndConstruct(*src->pRoute);
        routeMap.emplace(src->pRoute, cloned);
        dst->pRoute = cloned;
      } else {
        dst->pRoute = it->second;
      }
    } else {
      dst->pRoute = nullptr;
    }

    // Clone children
    dst->children.clear();
    dst->children.reserve(src->children.size());
    for (const RadixNode* srcChild : src->children) {
      RadixNode* dstChild = _nodePool.allocateAndConstruct();
      dst->children.push_back(dstChild);
      workQueue.emplace_back(srcChild, dstChild);
    }
  }

  // Clone literal-only routes map (stored independently of the radix tree)
  _literalOnlyRoutes = other._literalOnlyRoutes;
}

void Router::clear() noexcept {
  _handler = {};
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  _asyncHandler = {};
#endif
  _streamingHandler = {};
  _globalPreMiddleware.clear();
  _globalPostMiddleware.clear();
  _nodePool.clear();
  _compiledRoutePool.clear();
  _pRootNode = nullptr;
  _literalOnlyRoutes.clear();
}

void Router::printTree(std::ostream& os) const {
  if (_pRootNode == nullptr) {
    os << "<empty router>\n";
    return;
  }

  os << "Radix tree\n";
  os << "==========\n";
  printNode(os, *_pRootNode, 0);
}

const char* Router::nodeTypeToString(NodeType nodeType) {
  switch (nodeType) {
    case NodeType::Param:
      return "PARAM";
    case NodeType::CatchAll:
      return "CATCHALL";
    default:
      return "STATIC";
  }
}

// NOLINTNEXTLINE(misc-no-recursion) - this is OK for a debugging function
void Router::printNode(std::ostream& os, const RadixNode& node, int depth) const {
  indent(os, depth);

  os << "└─ ";

  // Node header
  os << "[" << nodeTypeToString(node.nodeType) << "] ";
  os << '"' << std::string_view(node.path) << '"';

  if (node.hasWildChild) {
    os << "  (hasWildChild)";
  }

  if (node.pRoute != nullptr) {
    os << "  [ROUTE";
    if (node.pRoute->hasNoSlashRegistered) {
      os << " no-slash";
    }
    if (node.pRoute->hasWithSlashRegistered) {
      os << " with-slash";
    }
    if (node.pRoute->hasWildcard) {
      os << " wildcard";
    }
    os << "]";
  }

  if (node.handlers.hasAnyHandler()) {
    os << "  [handlers]";
  }

  os << '\n';

  // Children
  const std::size_t staticCount = node.indices.size();

  for (uint32_t childPos = 0; childPos < node.children.size(); ++childPos) {
    indent(os, depth + 1);

    if (childPos < staticCount) {
      os << "edge '" << node.indices[childPos] << "'\n";
    } else {
      os << "edge <wildcard>\n";
    }

    printNode(os, *node.children[childPos], depth + 2);
  }
}

}  // namespace aeronet
