#include "aeronet/router.hpp"

#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/http-method.hpp"
#include "aeronet/router-config.hpp"
#include "exception.hpp"
#include "log.hpp"

namespace aeronet {

namespace {

bool shouldNormalize(RouterConfig::TrailingSlashPolicy policy, auto path) {
  return policy != RouterConfig::TrailingSlashPolicy::Strict && path.size() > 1 && path.back() == '/';
}

}  // namespace

Router::Router(const RouterConfig& config) : _trailingSlashPolicy(config.trailingSlashPolicy) {}

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

void Router::setPath(std::string path, http::MethodBmp methods, RequestHandler handler) {
  const bool doNormalize = shouldNormalize(_trailingSlashPolicy, path);
  if (doNormalize) {
    path.pop_back();
  }

  auto [it, inserted] = _pathHandlers.emplace(std::move(path), PathHandlerEntry{});

  if (!inserted && (methods & it->second.normalMethodBmp) != 0) {
    log::warn("Overwriting existing path handler for path '{}'", it->first);
  }

  PathHandlerEntry* pEntry = &it->second;

  pEntry->normalMethodBmp |= methods;
  pEntry->isNormalized = doNormalize;

  RequestHandler* pHandler = nullptr;
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    if (http::isMethodSet(methods, methodIdx)) {
      if (pEntry->streamingHandlers[methodIdx]) {
        throw exception("Cannot register normal handler: streaming handler already present for path+method");
      }
      if (pHandler == nullptr) {
        pEntry->normalHandlers[methodIdx] = std::move(handler);  // NOLINT(bugprone-use-after-move)
        pHandler = &pEntry->normalHandlers[methodIdx];
      } else {
        pEntry->normalHandlers[methodIdx] = *pHandler;
      }
    }
  }
}

void Router::setPath(std::string path, http::Method method, RequestHandler handler) {
  setPath(std::move(path), static_cast<http::MethodBmp>(method), std::move(handler));
}

void Router::setPath(std::string path, http::MethodBmp methods, StreamingHandler handler) {
  const bool doNormalize = shouldNormalize(_trailingSlashPolicy, path);
  if (doNormalize) {
    path.pop_back();
  }

  auto [it, inserted] = _pathHandlers.emplace(std::move(path), PathHandlerEntry{});

  if (!inserted && (methods & it->second.streamingMethodBmp) != 0) {
    log::warn("Overwriting existing streaming path handler for path '{}'", it->first);
  }

  PathHandlerEntry* pEntry = &it->second;

  pEntry->streamingMethodBmp |= methods;
  pEntry->isNormalized = doNormalize;

  StreamingHandler* pHandler = nullptr;
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    if (http::isMethodSet(methods, methodIdx)) {
      if (pEntry->normalHandlers[methodIdx]) {
        throw exception("Cannot register streaming handler: normal handler already present for path+method");
      }
      if (pHandler == nullptr) {
        pEntry->streamingHandlers[methodIdx] = std::move(handler);  // NOLINT(bugprone-use-after-move)
        pHandler = &pEntry->streamingHandlers[methodIdx];
      } else {
        pEntry->streamingHandlers[methodIdx] = *pHandler;
      }
    }
  }
}

void Router::setPath(std::string path, http::Method method, StreamingHandler handler) {
  setPath(std::move(path), static_cast<http::MethodBmp>(method), std::move(handler));
}

Router::RoutingResult Router::match(http::Method method, std::string_view path) const {
  std::string_view normalizedPath = path;
  if (shouldNormalize(_trailingSlashPolicy, path)) {
    normalizedPath.remove_suffix(1);
  }

  RoutingResult res;

  // Provide implicit HEAD->GET fallback (RFC7231: HEAD is identical to GET without body) when
  // a HEAD handler is not explicitly registered but a GET handler exists for the same path.

  // 1. Always attempt exact match first, independent of policy.
  auto it = _pathHandlers.find(normalizedPath);
  if (it != _pathHandlers.end()) {
    // Trailing slash redirect behavior:
    // In Redirect mode we will return a redirect when the request's trailing-slash
    // form does not match the registered canonical form. This intentionally works
    // in both directions: if a handler was registered with a trailing slash and
    // the request omits it we redirect to the slashed form, and vice-versa.
    //
    // Note: in Normalize mode we treat paths differing only by a single
    // trailing '/' as the same canonical path. If a caller registers both the
    // normalized and non-normalized variants, only the first registration for a
    // given canonical path is kept (subsequent registrations target the same
    // canonical entry). This avoids surprising behavior where `/foo` and
    // `/foo/` would be routed to different handlers under Normalize.
    if (_trailingSlashPolicy == RouterConfig::TrailingSlashPolicy::Redirect) {
      if (it->second.isNormalized) {
        // we registered a path with a trailing slash
        if (normalizedPath.size() == path.size()) {
          // the path has no trailing slash
          // -> redirect to path with trailing slash
          res.redirectPathIndicator = RoutingResult::RedirectSlashMode::AddSlash;
          return res;
        }
        // the path has a trailing slash
        // -> no redirect needed
      } else {
        // we registered a path without a trailing slash
        if (normalizedPath.size() == path.size()) {
          // the path has no trailing slash
          // -> no redirect needed
        } else {
          // the path has a trailing slash
          // -> redirect to path without trailing slash
          res.redirectPathIndicator = RoutingResult::RedirectSlashMode::RemoveSlash;
          return res;
        }
      }
    }

    auto& entry = it->second;
    // If HEAD and no explicit HEAD handler, but GET handler exists, reuse GET handler index.
    auto methodIdxOriginal = toMethodIdx(method);
    auto methodIdx = methodIdxOriginal;
    if (method == http::Method::HEAD) {
      static constexpr auto kHeadIdx = toMethodIdx(http::Method::HEAD);
      static constexpr auto kGetIdx = toMethodIdx(http::Method::GET);
      if (!entry.streamingHandlers[kHeadIdx] && !entry.normalHandlers[kHeadIdx]) {
        if (entry.streamingHandlers[kGetIdx] || entry.normalHandlers[kGetIdx]) {
          method = http::Method::GET;
          methodIdx = kGetIdx;
        }
      }
    }
    if (entry.streamingHandlers[methodIdx] && http::isMethodSet(entry.streamingMethodBmp, method)) {
      res.streamingHandler = &entry.streamingHandlers[methodIdx];
    } else if (entry.normalHandlers[methodIdx] && http::isMethodSet(entry.normalMethodBmp, method)) {
      res.requestHandler = &entry.normalHandlers[methodIdx];
    } else {
      res.methodNotAllowed = true;
    }
    return res;
  }

  // path handler not found, trying global handlers
  if (_streamingHandler) {
    res.streamingHandler = &_streamingHandler;
  } else if (_handler) {
    res.requestHandler = &_handler;
  }

  return res;
}

http::MethodBmp Router::allowedMethods(std::string_view path) const {
  std::string_view normalizedPath = path;
  if (shouldNormalize(_trailingSlashPolicy, path)) {
    normalizedPath.remove_suffix(1);
  }

  const auto it = _pathHandlers.find(normalizedPath);
  if (it != _pathHandlers.end()) {
    const auto& entry = it->second;
    return static_cast<http::MethodBmp>(entry.normalMethodBmp | entry.streamingMethodBmp);
  }
  // No path-specific handler: if global handlers exist treat them as allowing all methods
  if (_streamingHandler || _handler) {
    // Allow all known methods
    static constexpr http::MethodBmp kAllMethods = (1U << http::kNbMethods) - 1U;
    return kAllMethods;
  }
  return 0;
}

}  // namespace aeronet