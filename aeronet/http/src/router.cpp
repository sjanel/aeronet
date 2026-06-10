#include "aeronet/router.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
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
  if (policy == RouterConfig::TrailingSlashPolicy::Strict) {
    return pathHasTrailingSlash;
  }
  path.remove_suffix(static_cast<std::string_view::size_type>(pathHasTrailingSlash));
  return pathHasTrailingSlash;
}

void Indent(std::ostream& os, int depth) {
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
  auto [itA, itB] = std::ranges::mismatch(pathA, pathB);
  return static_cast<std::size_t>(itA - pathA.begin());
}

// Find the position of a wildcard (:param or {param} or *) in the path
// Returns {start_position, end_position} of the wildcard, or npos if none found
struct WildcardInfo {
  std::size_t first{std::string_view::npos};
  std::size_t last{std::string_view::npos};
};

// Extract the constraint pattern from a segment path like "{name:pattern}..." or "{name}..."
// Returns the constraint pattern text (empty if unconstrained).
std::string_view ExtractConstraintPattern(std::string_view segPath) noexcept {
  assert(!segPath.empty());
  assert(segPath[0] == '{');
  // Find ':' separator first
  const auto colonPos = segPath.find(':');
  if (colonPos == std::string_view::npos) {
    return {};  // No constraint
  }
  // Find matching '}' with brace depth counting (constraint patterns may have {n} quantifiers)
  int depth = 1;
  std::size_t closePos = 1;
  for (; closePos < segPath.size(); ++closePos) {
    if (segPath[closePos] == '{') {
      ++depth;
    } else if (segPath[closePos] == '}') {
      --depth;
      if (depth == 0) {
        break;
      }
    }
  }
  // Extract constraint pattern (after ':' up to closing '}')
  return segPath.substr(colonPos + 1, closePos - colonPos - 1);
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

Router::Route Router::ParsePattern(std::string_view path) {
  assert(!path.empty());
  if (path.front() != '/') {
    throw std::invalid_argument("Router paths must begin with '/'");
  }

  Route route;
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

      // Find matching '}' — strategy depends on whether there's a constraint pattern
      // First, check if there's a ':' before the first unescaped '}' (indicating a constraint)
      std::size_t closePos = std::string_view::npos;
      std::size_t colonPos2 = std::string_view::npos;
      for (std::size_t sp = idx + 1; sp < segment.size(); ++sp) {
        if (segment[sp] == ':' && colonPos2 == std::string_view::npos) {
          colonPos2 = sp;
        } else if (segment[sp] == '}') {
          if (colonPos2 == std::string_view::npos) {
            // No constraint — use FindUnescaped-style logic: skip }}
            if (sp + 1 < segment.size() && segment[sp + 1] == '}') {
              ++sp;  // Skip escaped }}
              continue;
            }
            closePos = sp;
            break;
          }
          // Has constraint — use brace-depth counting from the colon onwards
          break;
        }
      }

      if (colonPos2 != std::string_view::npos) {
        // Constraint pattern present — find matching '}' with brace-depth counting
        int braceDepth = 1;
        for (std::size_t sp = idx + 1; sp < segment.size(); ++sp) {
          if (segment[sp] == '{') {
            ++braceDepth;
          } else if (segment[sp] == '}') {
            --braceDepth;
            if (braceDepth == 0) {
              closePos = sp;
              break;
            }
          }
        }
      }

      if (closePos == std::string_view::npos) {
        throw std::invalid_argument("Unterminated '{' in router pattern");
      }

      if (previousWasParam) {
        throw std::invalid_argument("Consecutive parameters without separator are not allowed");
      }
      previousWasParam = true;

      const std::string_view paramContent = segment.substr(idx + 1U, closePos - idx - 1U);

      // Split on first ':' to separate name from constraint pattern
      const auto colonPos = paramContent.find(':');
      std::string_view paramName;
      std::string_view constraintPattern;

      if (colonPos != std::string_view::npos) {
        paramName = paramContent.substr(0, colonPos);
        constraintPattern = paramContent.substr(colonPos + 1);
      } else {
        paramName = paramContent;
      }

      // Reject nested braces in param names (only allowed in constraint patterns)
      // Delimiter scanning above guarantees no unescaped '}' can appear in paramName:
      // any '}' before closePos is either part of an escaped '}}' pair or the delimiter itself.
      if (paramName.contains('{')) {
        throw std::invalid_argument("Invalid character in parameter name");
      }

      if (paramName.empty()) {
        sawUnnamed = true;
        route.paramNames.append(std::string_view(IntegralToCharVector(paramIdx)));
      } else {
        sawNamed = true;
        route.paramNames.append(paramName);
      }

      // Compile constraint (empty pattern = unconstrained)
      route.paramConstraints.emplace_back(constraintPattern);

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

  ++children[static_cast<uint32_t>(pos)]->priority;
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
    std::rotate(indices.begin() + static_cast<std::ptrdiff_t>(newPos),
                indices.begin() + static_cast<std::ptrdiff_t>(pos),
                indices.begin() + static_cast<std::ptrdiff_t>(pos + 1));
  }

  return newPos;
}

Router::RadixNode* Router::insertChild(RadixNode& node, std::string_view path, const Route& route, uint32_t& paramIdx) {
  RadixNode* pNode = &node;

  while (true) {
    // Find position of first wildcard
    const auto [wildcardFirst, wildcardLast] = FindWildcard(path);

    if (wildcardFirst == std::string_view::npos) {
      // No wildcard found, simply store the path
      pNode->path = unescapeAndAllocate(path);
      return pNode;
    }

    // Handle param: {name} or {name}literal or prefix{name}
    const std::string_view wildcard(path.data() + wildcardFirst, path.data() + wildcardLast);
    assert(!wildcard.empty());
    if (wildcard[0] == '{') {
      if (wildcardFirst > 0) {
        // Insert prefix before the wildcard
        pNode->path = unescapeAndAllocate(path.substr(0, wildcardFirst));
        path = path.substr(wildcardFirst);
      }

      // Find the end of the segment containing the param
      const std::size_t segEnd = path.find('/');
      const std::string_view paramSegment = segEnd == std::string_view::npos ? path : path.substr(0, segEnd);

      // Parse the param segment into parts
      pNode->hasWildChild = true;
      RadixNode* pChild = _nodePool.allocateAndConstruct();
      pChild->nodeType = NodeType::Param;
      pChild->path = unescapeAndAllocate(paramSegment);

      char* currentLiteral = nullptr;
      std::size_t currentLiteralSize = 0;
      std::size_t currentLiteralCapacity = 0;

      auto flushLiteral = [&]() {
        if (currentLiteral == nullptr) {
          return;
        }

        _charStorage.shrinkLastAllocated(currentLiteral, currentLiteralSize);
        pChild->paramParts.push_back({.literal = {currentLiteral, currentLiteralSize}});
        currentLiteral = nullptr;
        currentLiteralSize = 0;
        currentLiteralCapacity = 0;
      };

      auto appendLiteralChar = [&](char ch, std::size_t literalUpperBound) {
        if (currentLiteral == nullptr) {
          assert(literalUpperBound > 0);
          currentLiteralCapacity = literalUpperBound;
          currentLiteral = _charStorage.allocateAndDefaultConstruct(currentLiteralCapacity);
        }

        assert(currentLiteralSize < currentLiteralCapacity);
        currentLiteral[currentLiteralSize] = ch;
        ++currentLiteralSize;
      };

      // Parse the segment parts (literal/param alternation)
      for (std::size_t idx = 0; idx < paramSegment.size();) {
        // Handle escaped braces
        if (paramSegment.compare(idx, kEscapedOpenBrace.size(), kEscapedOpenBrace) == 0) {
          appendLiteralChar('{', paramSegment.size() - idx);
          idx += kEscapedOpenBrace.size();
          continue;
        }
        if (paramSegment.compare(idx, kEscapedCloseBrace.size(), kEscapedCloseBrace) == 0) {
          appendLiteralChar('}', paramSegment.size() - idx);
          idx += kEscapedCloseBrace.size();
          continue;
        }

        if (paramSegment[idx] != '{') {
          // Literal character
          appendLiteralChar(paramSegment[idx], paramSegment.size() - idx);
          ++idx;
          continue;
        }

        // Parameter - find matching '}' (strategy depends on whether there's a constraint ':')
        // Check for ':' before first '}' to determine if constraint pattern is present
        const auto delimiterPos = paramSegment.find_first_of(":}", idx + 1U);
        assert(delimiterPos != std::string_view::npos);
        const bool hasConstraint = paramSegment[delimiterPos] == ':';

        std::size_t closePos = idx + 1;
        if (hasConstraint) {
          // Brace-depth counting for constraint patterns with {n} quantifiers
          int depth = 1;
          for (;; ++closePos) {
            assert(closePos < paramSegment.size());
            if (paramSegment[closePos] == '{') {
              ++depth;
            } else if (paramSegment[closePos] == '}') {
              --depth;
              if (depth == 0) {
                break;
              }
            }
          }
        } else {
          // FindUnescaped-style: skip }} escape sequences
          for (;;) {
            assert(closePos < paramSegment.size());
            if (paramSegment[closePos] == '}') {
              if (closePos + 1 < paramSegment.size() && paramSegment[closePos + 1] == '}') {
                closePos += 2;  // Skip escaped }} and move to next character
                continue;
              }
              break;
            }
            ++closePos;
          }
        }
        assert(closePos < paramSegment.size());  // Already validated by ParsePattern

        // should have been caught by ParsePattern
        flushLiteral();
        pChild->paramParts.emplace_back();
        assert(paramIdx < route.paramConstraints.size());
        pChild->segmentConstraints.emplace_back(route.paramConstraints[paramIdx].pattern(), _charStorage);
        ++paramIdx;
        idx = closePos + 1U;
      }

      flushLiteral();

      // For single-param segments, store constraint on the node for efficient matching
      if (pChild->paramParts.size() == 1) {
        assert(paramIdx > 0);
        assert(paramIdx - 1 < route.paramConstraints.size());
        pChild->constraint = RouteConstraint(route.paramConstraints[paramIdx - 1].pattern(), _charStorage);
      }

      const auto firstWildcardChild = pNode->children.begin() + static_cast<ptrdiff_t>(pNode->indices.size());
      const auto insertPos = [&]() {
        if (!pChild->constraint.empty()) {
          // Constrained params stay before unconstrained params and before any catch-all.
          return std::ranges::find_if(firstWildcardChild, pNode->children.end(), [](const RadixNode* pChild) {
            return pChild->nodeType != NodeType::Param || pChild->constraint.empty();
          });
        }
        return std::ranges::find_if(firstWildcardChild, pNode->children.end(),
                                    [](const RadixNode* pChild) { return pChild->nodeType == NodeType::CatchAll; });
      }();
      pNode->children.insert(insertPos, pChild);
      pNode = pChild;
      ++pNode->priority;

      // If path doesn't end with the param segment, continue with remaining path
      if (segEnd != std::string_view::npos) {
        path = path.substr(segEnd);
        RadixNode* pNextChild = _nodePool.allocateAndConstruct();
        pNextChild->priority = 1;
        pNode->children.push_back(pNextChild);
        pushBackIndex(*pNode, '/');
        pNode = pNextChild;
        continue;
      }
      return pNode;
    }

    // Store path up to (but not including) the '/*'
    assert(wildcardFirst > 0);
    pNode->path = unescapeAndAllocate(path.substr(0, wildcardFirst - 1));  // Exclude the trailing '/'

    // Create catch-all node
    // Note: We don't add catch-all to indices - it's accessed via hasWildChild
    pNode->hasWildChild = true;
    RadixNode* pCatchAllNode = _nodePool.allocateAndConstruct();
    pCatchAllNode->nodeType = NodeType::CatchAll;
    pCatchAllNode->path = allocatePath("/*");
    pNode->children.push_back(pCatchAllNode);
    // Do NOT add to indices: currentNode->indices.push_back('/');

    return pCatchAllNode;
  }
}

Router::RadixNode* Router::insertRoute(std::string_view path, Route&& route, bool pathHasTrailingSlash) {
  if (_pRootNode == nullptr) {
    _pRootNode = _nodePool.allocateAndConstruct();
  }

  RadixNode* pNode = _pRootNode;
  ++pNode->priority;
  uint32_t paramIdx = 0;

  // Empty tree case
  if (pNode->path.empty()) {
    assert(pNode->indices.empty());
    pNode = insertChild(*pNode, path, route, paramIdx);

    // Store route metadata
    pNode->pRoute = _compiledRoutePool.allocateAndConstruct();
    pNode->pRoute->paramNames = std::move(route.paramNames);
    pNode->pRoute->paramConstraints = std::move(route.paramConstraints);
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
      RadixNode& child = *_nodePool.allocateAndConstruct();
      child.path = currentPath.substr(commonPrefixLen);
      child.indices = pNode->indices;
      child.priority = pNode->priority - 1;
      child.hasWildChild = pNode->hasWildChild;
      child.paramParts = std::move(pNode->paramParts);
      child.children = std::move(pNode->children);
      child.handlers = std::move(pNode->handlers);
      child.pRoute = pNode->pRoute;

      // Current node becomes the common prefix
      pNode->children = {&child};
      pNode->indices = {};
      pushBackIndex(*pNode, currentPath[commonPrefixLen]);
      pNode->path = currentPath.substr(0, commonPrefixLen);
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
      const std::string_view indices(pNode->indices.data(), pNode->indices.size());
      const auto foundPos = indices.find(firstChar);
      if (foundPos != std::string_view::npos) {
        pNode = pNode->children[static_cast<uint32_t>(pNode->incrementChildPrio(foundPos))];
        continue;
      }

      // No static match found. If we have a wildcard child and the path starts with a wildcard,
      // try to use it. Also check for catch-all pattern "/*" or the post-prefix remainder "*".
      if (pNode->hasWildChild && (firstChar == '{' || path == "*" || path == "/*")) {
        // Wildcard children are at the end of children array (after static children)
        // Static children count = indices.size()
        const auto staticCount = indices.size();
        const auto totalChildren = pNode->children.size();
        bool matched = false;

        for (auto wildIdx = staticCount; wildIdx < totalChildren; ++wildIdx) {
          RadixNode* pWildcardChild = pNode->children[static_cast<uint32_t>(wildIdx)];
          assert(pWildcardChild->nodeType != NodeType::Static);

          // Special case for catch-all: if both are catch-all, allow re-registration
          if (pWildcardChild->nodeType == NodeType::CatchAll) {
            if (path != "*" && path != "/*") {
              continue;
            }
            assert(pWildcardChild->pRoute != nullptr);
            assert(pWildcardChild->pRoute->hasWildcard);
            if (pathHasTrailingSlash) {
              pWildcardChild->pRoute->hasWithSlashRegistered = true;
            } else {
              pWildcardChild->pRoute->hasNoSlashRegistered = true;
            }
            return pWildcardChild;
          }

          const std::string_view nodePath = pWildcardChild->path;
          if (!path.starts_with(nodePath)) {
            continue;
          }
          // Wildcard node paths are whole-segment patterns. If there is remaining text,
          // it must start at the next segment boundary. Otherwise this is a different
          // wildcard shape that should fall through to conflict handling below.
          if (nodePath.size() < path.size() && path[nodePath.size()] != '/') {
            continue;
          }

          pNode = pWildcardChild;
          ++pNode->priority;
          matched = true;
          paramIdx += static_cast<uint32_t>(std::ranges::count_if(
              pNode->paramParts, [](const SegmentPart& part) { return part.kind() == SegmentPart::Kind::Param; }));
          break;
        }

        if (matched) {
          continue;
        }

        // No existing wildcard child matches by path prefix.
        // Check if this is a true conflict (same constraint, different param names) vs
        // a legitimate alternative (different constraint).
        if (firstChar == '{') {
          const auto newConstraintPattern = ExtractConstraintPattern(path);
          for (auto wildIdx = staticCount; wildIdx < totalChildren; ++wildIdx) {
            const RadixNode* pWildcardChild = pNode->children[static_cast<uint32_t>(wildIdx)];
            if (pWildcardChild->nodeType != NodeType::Param) {
              continue;
            }

            const auto existingConstraintPattern = ExtractConstraintPattern(pWildcardChild->path);
            if (newConstraintPattern == existingConstraintPattern) {
              throw std::invalid_argument("Path conflicts with existing wildcard route");
            }
          }
        }
        // Different constraint - fall through to insert a new wildcard child
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
      if (firstChar != '{' && path != "*" && path != "/*") {
        pushBackIndex(*pNode, firstChar);
        RadixNode* pChild = _nodePool.allocateAndConstruct();
        // If there are wildcard children, they must stay at the end.
        // Insert static child before all wildcard children
        if (pNode->hasWildChild) {
          assert(!pNode->children.empty());
          const auto insertPos = pNode->children.begin() + static_cast<ptrdiff_t>(pNode->indices.size() - 1);
          pNode->children.insert(insertPos, pChild);
        } else {
          pNode->children.push_back(pChild);
        }
        pNode->incrementChildPrio(pNode->indices.size() - 1);
        pNode = pChild;
      }

      if (path == "*" || path == "/*") {
        pNode->hasWildChild = true;
        RadixNode* pCatchAllChild = _nodePool.allocateAndConstruct();
        pCatchAllChild->nodeType = NodeType::CatchAll;
        pCatchAllChild->path = allocatePath("/*");
        pNode->children.push_back(pCatchAllChild);
        pNode = pCatchAllChild;
      } else {
        pNode = insertChild(*pNode, path, route, paramIdx);
      }

      // Store route metadata
      assert(pNode->pRoute == nullptr);
      pNode->pRoute = _compiledRoutePool.allocateAndConstruct();
      pNode->pRoute->paramNames = std::move(route.paramNames);
      pNode->pRoute->paramConstraints = std::move(route.paramConstraints);
      pNode->pRoute->hasWildcard = route.hasWildcard;

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
      pNode->pRoute->paramConstraints = std::move(route.paramConstraints);
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

  Route parsed = ParsePattern(path);
  const bool isLiteralOnly = parsed.paramNames.empty() && !parsed.hasWildcard;
  if (pathHasTrailingSlash) {
    parsed.hasWithSlashRegistered = true;
  } else {
    parsed.hasNoSlashRegistered = true;
  }

  PathHandlerEntry* pEntry;
  if (isLiteralOnly) {
    const std::string_view literalKey = unescapeAndAllocate(path);
    auto [it, inserted] = _literalOnlyRoutes.try_emplace(literalKey);
    if (!inserted) {
      _charStorage.shrinkLastAllocated(const_cast<char*>(literalKey.data()), 0);
    }
    LiteralRouteEntry& literalEntry = it->second;

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

bool Router::matchParamParts(std::span<const SegmentPart> parts, std::string_view segment,
                             std::span<const RouteConstraint> constraints) {
  std::size_t pos = 0;
  uint32_t constraintIdx = 0;

  for (uint32_t idx = 0; idx < parts.size(); ++idx) {
    const SegmentPart& part = parts[idx];

    if (part.kind() == SegmentPart::Kind::Literal) {
      assert(pos <= segment.size());
      // With current part construction and capture progression, literal parts are
      // reached only at already-aligned positions.
      assert(segment.substr(pos).starts_with(part.literal));
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

    const auto captured = segment.substr(captureStart, captureEnd - captureStart);

    // Validate constraint
    assert(constraintIdx < constraints.size());
    if (!constraints[constraintIdx].matches(captured, _uint32VectorBuffer)) {
      return false;
    }
    ++constraintIdx;

    _matchStateBuffer.push_back(captured);
  }

  return pos == segment.size();
}

const Router::RadixNode* Router::matchImpl(std::string_view path, bool requestHasTrailingSlash) {
  if (_pRootNode == nullptr) {
    return nullptr;
  }

  _matchStateBuffer.clear();
  const RadixNode* pNode = _pRootNode;
  const RadixNode* pCatchAllFallback = nullptr;
  decltype(_matchStateBuffer.size()) catchAllFallbackMatchStateSize = 0;

  auto rememberCatchAllFallback = [&](const RadixNode* node) {
    const auto wildcardIdx = node->indices.size();
    if (wildcardIdx >= node->children.size()) {
      return;
    }

    const RadixNode* pLastWildcardChild = node->children.back();
    if (pLastWildcardChild->nodeType != NodeType::CatchAll) {
      return;
    }
    assert(pLastWildcardChild->pRoute != nullptr);

    pCatchAllFallback = pLastWildcardChild;
    catchAllFallbackMatchStateSize = _matchStateBuffer.size();
  };

  auto fallbackToCatchAll = [&]() -> const RadixNode* {
    if (pCatchAllFallback == nullptr) {
      return nullptr;
    }

    _matchStateBuffer.resize(catchAllFallbackMatchStateSize);
    if (_config.trailingSlashPolicy == RouterConfig::TrailingSlashPolicy::Strict && requestHasTrailingSlash) {
      return nullptr;
    }

    assert(pCatchAllFallback->pRoute != nullptr);
    assert(_config.trailingSlashPolicy != RouterConfig::TrailingSlashPolicy::Strict ||
           pCatchAllFallback->pRoute->hasNoSlashRegistered);
    return pCatchAllFallback;
  };

  // Walk the tree
  while (true) {
    const std::string_view prefix = pNode->path;

    if (path.size() > prefix.size()) {
      if (path.starts_with(prefix)) {
        path = path.substr(prefix.size());
        const char firstChar = path[0];

        // First, try to match a static child by finding one whose path is a prefix of remaining path
        const std::string_view indices(pNode->indices.data(), pNode->indices.size());
        for (std::size_t idx = 0; idx < indices.size(); ++idx) {
          if (indices[idx] == firstChar) {
            const RadixNode* pChild = pNode->children[static_cast<uint32_t>(idx)];
            const std::string_view childPath = pChild->path;
            // Verify this child's path is actually a prefix of remaining path
            if (path.starts_with(childPath)) {
              rememberCatchAllFallback(pNode);
              pNode = pChild;
              goto continueWalk;
            }
            // Index matched but prefix didn't - fall through to try wildcard
          }
        }

        // No static match. Try wildcard children if present
        if (pNode->hasWildChild) {
          // Wildcard children are at the end of children array (after static children)
          const auto staticCount = pNode->indices.size();
          const auto totalChildren = pNode->children.size();

          // Find param end (either '/' or path end) - shared by all param children
          const std::size_t segEnd = path.find('/');
          const std::string_view segment = path.substr(0, segEnd);

          // Try each wildcard child in order (constrained first, unconstrained last)
          for (auto wildIdx = staticCount; wildIdx < totalChildren; ++wildIdx) {
            const RadixNode* pWildcardChild = pNode->children[static_cast<uint32_t>(wildIdx)];

            if (pWildcardChild->nodeType == NodeType::Param) {
              // Save match state buffer position for backtracking
              const auto savedBufferSize = _matchStateBuffer.size();

              // Validate constraint on the captured segment
              if (!pWildcardChild->constraint.empty() &&
                  !pWildcardChild->constraint.matches(segment, _uint32VectorBuffer)) {
                // Constraint failed - try next wildcard child
                continue;
              }

              // Match param pattern if we have parts
              assert(!pWildcardChild->paramParts.empty());
              const bool matched =
                  matchParamParts(pWildcardChild->paramParts, segment, pWildcardChild->segmentConstraints);

              if (!matched) {
                // Restore match state buffer
                _matchStateBuffer.resize(savedBufferSize);
                continue;
              }

              const RadixNode* pParamNode = pWildcardChild;

              // Continue to next segment if there's more path
              if (segEnd < path.size()) {
                if (!pParamNode->children.empty()) {
                  path = path.substr(segEnd);
                  pNode = pParamNode->children[0];
                  goto continueWalk;
                }
                // More path but no children - restore and try next alternative
                _matchStateBuffer.resize(savedBufferSize);
                continue;
              }

              // End of path
              if (pParamNode->pRoute != nullptr) {
                if (_config.trailingSlashPolicy != RouterConfig::TrailingSlashPolicy::Strict) {
                  return pParamNode;
                }
                // In this end-of-segment path, a trailing slash would have produced
                // a remaining "/..." child traversal above.
                assert(!requestHasTrailingSlash);
                assert(pParamNode->pRoute->hasNoSlashRegistered);
                return pParamNode;
              }

              // In non-strict policies, trailing slashes are normalized on registration,
              // so a standalone slash child is not expected to carry a terminal route.
              assert(_config.trailingSlashPolicy == RouterConfig::TrailingSlashPolicy::Strict ||
                     pParamNode->children.size() != 1 || pParamNode->children[0]->path != "/" ||
                     pParamNode->children[0]->pRoute == nullptr);

              // This param child matched structurally but has no handler at this position.
              // Restore and try next alternative.
              _matchStateBuffer.resize(savedBufferSize);
              continue;
            }

            assert(pWildcardChild->nodeType == NodeType::CatchAll);
            // Catch-all matches everything remaining
            pNode = pWildcardChild;
            assert(pNode->pRoute != nullptr);
            if (_config.trailingSlashPolicy != RouterConfig::TrailingSlashPolicy::Strict) {
              return pNode;
            }
            if (requestHasTrailingSlash) {
              return nullptr;
            }

            // Catch-all patterns strictly end in '*' (e.g. "/*"), never in '/'.
            // Because of this, pathHasTrailingSlash is always false during their registration,
            // which guarantees that pRoute->hasNoSlashRegistered is set to true.
            assert(pNode->pRoute->hasNoSlashRegistered);
            return pNode;
          }
        }

        // No matching child found - this path segment does not match any registered route.
        // Return nullptr so the caller can fall back to the default handler (e.g. static files).
        return fallbackToCatchAll();
      }
    } else if (path == prefix) {
      // Exact match - we should have reached the node containing the handler
      if (pNode->pRoute != nullptr) {
        if (_config.trailingSlashPolicy != RouterConfig::TrailingSlashPolicy::Strict) {
          return pNode;
        }
        // In strict mode, reaching exact node match implies registration exists for this
        // request slash variant at this node.
        assert(requestHasTrailingSlash ? pNode->pRoute->hasWithSlashRegistered : pNode->pRoute->hasNoSlashRegistered);
        return pNode;
      }

      // Wildcard children are ordered as params then optional catch-all.
      // So if any wildcard child exists and the last one is catch-all, that's the only
      // exact-match fallback candidate at this node.
      const auto wildcardIdx = pNode->indices.size();
      if (wildcardIdx < pNode->children.size()) {
        const RadixNode* pLastWildcardChild = pNode->children.back();
        if (pLastWildcardChild->nodeType == NodeType::CatchAll) {
          assert(pLastWildcardChild->pRoute != nullptr);
          // If we had a trailing slash on the request, but the CatchAll was registered
          // without it (handled automatically at registration), Strict mode should reject it.
          if (_config.trailingSlashPolicy != RouterConfig::TrailingSlashPolicy::Strict || !requestHasTrailingSlash) {
            return pLastWildcardChild;
          }
        }
      }
    }

    // Nothing found
    if (const RadixNode* pFallback = fallbackToCatchAll(); pFallback != nullptr) {
      return pFallback;
    }

    // Outside strict mode, registration strips trailing slashes, so the only node whose
    // prefix exceeds the remaining path by exactly one trailing '/' is an intermediate split node.
    assert(_config.trailingSlashPolicy == RouterConfig::TrailingSlashPolicy::Strict ||
           prefix.size() != path.size() + 1 || !prefix.starts_with(path) || prefix[path.size()] != '/' ||
           pNode->pRoute == nullptr);

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
        // Strict mode keeps slash and no-slash literals under distinct map keys, so a successful
        // exact lookup must have the matching registration bit set on the found entry.
        assert(pathHasTrailingSlash ? literalEntry.hasWithSlashRegistered : literalEntry.hasNoSlashRegistered);
        entryPtr = &literalEntry.handlers;
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
          } else {
            assert(literalEntry.hasNoSlashRegistered);
            result.redirectPathIndicator = RoutingResult::RedirectSlashMode::RemoveSlash;
          }
        } else {
          if (literalEntry.hasNoSlashRegistered) {
            entryPtr = &literalEntry.handlers;
          } else {
            assert(literalEntry.hasWithSlashRegistered);
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
    result.pathConfig = _defaultPathConfig;
    return result;
  }

  const PathHandlerEntry* entryPtr =
      computePathHandlerEntry(*pMatchedNode, pathHasTrailingSlash, result.redirectPathIndicator);
  if (entryPtr == nullptr) {
    return result;
  }

  setMatchedHandler(method, *entryPtr, result);

  // Build path params from match state
  const Route* pRoute = pMatchedNode->pRoute;
  if (pRoute != nullptr) {
    assert(std::cmp_equal(pRoute->paramNames.nbConcatenatedStrings(), _matchStateBuffer.size()));

    _pathParamCaptureBuffer.clear();
    for (auto [paramPos, param] : std::views::enumerate(pRoute->paramNames)) {
      _pathParamCaptureBuffer.emplace_back(param, _matchStateBuffer[static_cast<uint32_t>(paramPos)]);
    }
    result.pPathParams = _pathParamCaptureBuffer.data();
    result.nbPathParams = _pathParamCaptureBuffer.size();
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
  const Route* pRoute = matchedNode.pRoute;

  switch (_config.trailingSlashPolicy) {
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
      // matchImpl only returns route-bearing nodes here; handler sets may still be empty
      // if registration threw after the route metadata was created.
      assert(pRoute != nullptr);
      if (pathHasTrailingSlash) {
        if (pRoute->hasWithSlashRegistered) {
          return &matchedNode.handlers;
        }
        assert(pRoute->hasNoSlashRegistered);
        redirectSlashMode = RoutingResult::RedirectSlashMode::RemoveSlash;
      } else {
        if (pRoute->hasNoSlashRegistered) {
          return &matchedNode.handlers;
        }
        assert(pRoute->hasWithSlashRegistered);
        redirectSlashMode = RoutingResult::RedirectSlashMode::AddSlash;
      }
      break;
    default:
      // In Strict mode matchImpl only returns route-bearing nodes for the requested slash variant.
      assert(_config.trailingSlashPolicy == RouterConfig::TrailingSlashPolicy::Strict);
      assert(pRoute != nullptr);
      assert(pathHasTrailingSlash ? pRoute->hasWithSlashRegistered : pRoute->hasNoSlashRegistered);
      return &matchedNode.handlers;
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

  result.pPreMiddleware = entry._preMiddleware.data();
  result.nbPreMiddleware = entry._preMiddleware.size();

  result.pPostMiddleware = entry._postMiddleware.data();
  result.nbPostMiddleware = entry._postMiddleware.size();

  result.pathConfig = entry._pathConfig;
}

// ============================================================================
// Clone and Clear
// ============================================================================

void Router::cloneNodesFrom(const Router& other) {
  _nodePool.clear();
  _compiledRoutePool.clear();
  _literalOnlyRoutes.clear();
  _charStorage.clear();

  flat_hash_map<const Route*, Route*> routeMap;
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

    dst->priority = src->priority;
    dst->nodeType = src->nodeType;
    dst->hasWildChild = src->hasWildChild;
    dst->handlers = src->handlers;
    dst->path = src->path.empty() ? std::string_view{} : allocatePath(src->path);
    dst->indices = allocateIndices(std::string_view(src->indices.data(), src->indices.size()));
    dst->paramParts = src->paramParts;
    for (auto& part : dst->paramParts) {
      if (part.kind() == SegmentPart::Kind::Literal) {
        part.literal = allocatePath(part.literal);
      }
    }
    dst->constraint =
        src->constraint.empty() ? RouteConstraint{} : RouteConstraint(src->constraint.pattern(), _charStorage);
    dst->segmentConstraints.clear();
    dst->segmentConstraints.reserve(src->segmentConstraints.size());
    for (const auto& constraint : src->segmentConstraints) {
      dst->segmentConstraints.emplace_back(constraint.pattern(), _charStorage);
    }

    // Each radix node owns its compiled route. Splits move ownership to the child,
    // so the source tree must not contain two nodes sharing the same Route*.
    if (src->pRoute != nullptr) {
      assert(routeMap.find(src->pRoute) == routeMap.end());
      Route* cloned = _compiledRoutePool.allocateAndConstruct();
      cloned->paramNames = src->pRoute->paramNames;
      cloned->paramConstraints.reserve(src->pRoute->paramConstraints.size());
      for (const auto& constraint : src->pRoute->paramConstraints) {
        cloned->paramConstraints.emplace_back(constraint.pattern(), _charStorage);
      }
      cloned->hasWildcard = src->pRoute->hasWildcard;
      cloned->hasNoSlashRegistered = src->pRoute->hasNoSlashRegistered;
      cloned->hasWithSlashRegistered = src->pRoute->hasWithSlashRegistered;
      routeMap.emplace(src->pRoute, cloned);
      dst->pRoute = cloned;
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
  _literalOnlyRoutes.reserve(other._literalOnlyRoutes.size());
  for (const auto& [key, entry] : other._literalOnlyRoutes) {
    _literalOnlyRoutes.emplace(allocatePath(key), entry);
  }
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
  _literalOnlyRoutes.clear();
  _charStorage.clear();
  _pRootNode = nullptr;
}

std::string_view Router::unescapeAndAllocate(std::string_view input) {
  if (input.empty()) {
    return {};
  }
  char* dst = _charStorage.allocateAndDefaultConstruct(input.size());
  char* pDst = dst;
  for (std::size_t i = 0; i < input.size(); ++i) {
    *pDst = input[i];
    if ((input[i] == '{' || input[i] == '}') && i + 1 < input.size() && input[i + 1] == input[i]) {
      ++i;
    }
    ++pDst;
  }
  const auto finalSize = static_cast<std::size_t>(pDst - dst);
  _charStorage.shrinkLastAllocated(dst, finalSize);
  return {dst, finalSize};
}

std::string_view Router::allocatePath(std::string_view pathFragment) {
  assert(!pathFragment.empty());
  char* dst = _charStorage.allocateAndDefaultConstruct(pathFragment.size());
  std::memcpy(dst, pathFragment.data(), pathFragment.size());
  return {dst, pathFragment.size()};
}

std::span<char> Router::allocateIndices(std::string_view ind) {
  if (ind.empty()) {
    return {};
  }
  char* dst = _charStorage.allocateAndDefaultConstruct(ind.size());
  std::memcpy(dst, ind.data(), ind.size());
  return {dst, ind.size()};
}

void Router::pushBackIndex(RadixNode& node, char indexChar) {
  const auto oldSize = node.indices.size();
  char* newBuf = _charStorage.allocateAndDefaultConstruct(oldSize + 1);
  if (oldSize > 0) {
    std::memcpy(newBuf, node.indices.data(), oldSize);
  }
  newBuf[oldSize] = indexChar;
  node.indices = std::span<char>(newBuf, oldSize + 1);
}

namespace {
constexpr uint32_t kSentinelHeaderBytes = static_cast<uint32_t>(-1);
constexpr std::size_t kSentinelBodyBytes = static_cast<std::size_t>(-1);

void CheckRouteConfig(const PathEntryConfig& cfg, uint32_t globalMaxHeaderBytes, std::size_t globalMaxBodyBytes,
                      std::string_view routePath) {
  if (cfg.maxHeaderBytes != kSentinelHeaderBytes && cfg.maxHeaderBytes > globalMaxHeaderBytes) {
    log::critical("Per-route maxHeaderBytes ({}) for '{}' exceeds global limit ({})", cfg.maxHeaderBytes, routePath,
                  globalMaxHeaderBytes);
    throw std::invalid_argument("Per-route maxHeaderBytes should not exceed global limit");
  }
  if (cfg.maxBodyBytes != kSentinelBodyBytes && cfg.maxBodyBytes > globalMaxBodyBytes) {
    log::critical("Per-route maxBodyBytes ({}) for '{}' exceeds global limit ({})", cfg.maxBodyBytes, routePath,
                  globalMaxBodyBytes);
    throw std::invalid_argument("Per-route maxBodyBytes should not exceed global limit");
  }
}

void ClampConfig(PathEntryConfig& cfg, uint32_t globalMaxHeaderBytes, std::size_t globalMaxBodyBytes) noexcept {
  cfg.maxHeaderBytes = std::min(cfg.maxHeaderBytes, globalMaxHeaderBytes);
  cfg.maxBodyBytes = std::min(cfg.maxBodyBytes, globalMaxBodyBytes);
}
}  // namespace

void Router::clampConfigs(uint32_t globalMaxHeaderBytes, std::size_t globalMaxBodyBytes) {
  // Validate and clamp radix tree nodes iteratively.
  if (_pRootNode != nullptr) {
    vector<RadixNode*> stack(1U, _pRootNode);
    while (!stack.empty()) {
      auto* node = stack.back();
      stack.pop_back();

      CheckRouteConfig(node->handlers._pathConfig, globalMaxHeaderBytes, globalMaxBodyBytes, node->path);

      ClampConfig(node->handlers._pathConfig, globalMaxHeaderBytes, globalMaxBodyBytes);

      std::ranges::copy(node->children, std::back_inserter(stack));
    }
  }

  // Validate and clamp literal routes.
  for (auto& [key, entry] : _literalOnlyRoutes) {
    CheckRouteConfig(entry.handlers._pathConfig, globalMaxHeaderBytes, globalMaxBodyBytes, key);
    ClampConfig(entry.handlers._pathConfig, globalMaxHeaderBytes, globalMaxBodyBytes);
  }

  // _defaultPathConfig represents "no per-route override" so always equals the global values.
  _defaultPathConfig.maxHeaderBytes = globalMaxHeaderBytes;
  _defaultPathConfig.maxBodyBytes = globalMaxBodyBytes;
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
      assert(nodeType == NodeType::Static);
      return "STATIC";
  }
}

// NOLINTNEXTLINE(misc-no-recursion) - this is OK for a debugging function
void Router::printNode(std::ostream& os, const RadixNode& node, int depth) const {
  Indent(os, depth);

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
    Indent(os, depth + 1);

    if (childPos < staticCount) {
      os << "edge '" << node.indices[childPos] << "'\n";
    } else {
      os << "edge <wildcard>\n";
    }

    printNode(os, *node.children[childPos], depth + 2);
  }
}

}  // namespace aeronet
