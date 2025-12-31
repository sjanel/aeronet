#include "aeronet/path-handler-entry.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handlers.hpp"

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-endpoint.hpp"
#endif

namespace aeronet {

PathHandlerEntry::PathHandlerEntry(const PathHandlerEntry& rhs)
    : _normalMethodBmp(rhs._normalMethodBmp),
      _streamingMethodBmp(rhs._streamingMethodBmp),
      _asyncMethodBmp(rhs._asyncMethodBmp),
#ifdef AERONET_ENABLE_WEBSOCKET
      _websocketEndpoint(rhs._websocketEndpoint ? std::make_unique<WebSocketEndpoint>(*rhs._websocketEndpoint)
                                                : nullptr),
#endif
      _corsPolicy(rhs._corsPolicy),
      _preMiddleware(rhs._preMiddleware),
      _postMiddleware(rhs._postMiddleware),
      _pathConfig(rhs._pathConfig) {
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    const HandlerStorage& rhsStorage = rhs._handlers[methodIdx];
    HandlerStorage& lhsStorage = _handlers[methodIdx];

    if (http::IsMethodIdxSet(_normalMethodBmp, methodIdx)) {
      std::construct_at(&reinterpret_cast<RequestHandler&>(lhsStorage),
                        reinterpret_cast<const RequestHandler&>(rhsStorage));
    } else if (http::IsMethodIdxSet(_asyncMethodBmp, methodIdx)) {
      std::construct_at(&reinterpret_cast<AsyncRequestHandler&>(lhsStorage),
                        reinterpret_cast<const AsyncRequestHandler&>(rhsStorage));
    } else if (http::IsMethodIdxSet(_streamingMethodBmp, methodIdx)) {
      std::construct_at(&reinterpret_cast<StreamingHandler&>(lhsStorage),
                        reinterpret_cast<const StreamingHandler&>(rhsStorage));
    }
  }
}

PathHandlerEntry::PathHandlerEntry(PathHandlerEntry&& rhs) noexcept
    :
#ifdef AERONET_ENABLE_WEBSOCKET
      _websocketEndpoint(std::move(rhs._websocketEndpoint)),
#endif
      _corsPolicy(std::move(rhs._corsPolicy)),
      _preMiddleware(std::move(rhs._preMiddleware)),
      _postMiddleware(std::move(rhs._postMiddleware)),
      _pathConfig(rhs._pathConfig) {
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    HandlerStorage& rhsStorage = rhs._handlers[methodIdx];
    HandlerStorage& lhsStorage = _handlers[methodIdx];

    if (http::IsMethodIdxSet(rhs._normalMethodBmp, methodIdx)) {
      std::construct_at(&reinterpret_cast<RequestHandler&>(lhsStorage),
                        std::move(reinterpret_cast<RequestHandler&>(rhsStorage)));
    } else if (http::IsMethodIdxSet(rhs._asyncMethodBmp, methodIdx)) {
      std::construct_at(&reinterpret_cast<AsyncRequestHandler&>(lhsStorage),
                        std::move(reinterpret_cast<AsyncRequestHandler&>(rhsStorage)));
    } else if (http::IsMethodIdxSet(rhs._streamingMethodBmp, methodIdx)) {
      std::construct_at(&reinterpret_cast<StreamingHandler&>(lhsStorage),
                        std::move(reinterpret_cast<StreamingHandler&>(rhsStorage)));
    }
  }

  _normalMethodBmp = std::exchange(rhs._normalMethodBmp, 0U);
  _streamingMethodBmp = std::exchange(rhs._streamingMethodBmp, 0U);
  _asyncMethodBmp = std::exchange(rhs._asyncMethodBmp, 0U);
}

PathHandlerEntry& PathHandlerEntry::operator=(const PathHandlerEntry& rhs) {
  if (&rhs != this) [[likely]] {
#ifdef AERONET_ENABLE_WEBSOCKET
    _websocketEndpoint =
        rhs._websocketEndpoint ? std::make_unique<WebSocketEndpoint>(*rhs._websocketEndpoint) : nullptr;
#endif
    _corsPolicy = rhs._corsPolicy;
    _preMiddleware = rhs._preMiddleware;
    _postMiddleware = rhs._postMiddleware;
    _pathConfig = rhs._pathConfig;

    for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
      HandlerStorage& lhsStorage = _handlers[methodIdx];
      const HandlerStorage& rhsStorage = rhs._handlers[methodIdx];

      if (http::IsMethodIdxSet(rhs._normalMethodBmp, methodIdx)) {
        if (http::IsMethodIdxSet(_normalMethodBmp, methodIdx)) {
          reinterpret_cast<RequestHandler&>(lhsStorage) = reinterpret_cast<const RequestHandler&>(rhsStorage);
        } else {
          std::construct_at(&reinterpret_cast<RequestHandler&>(lhsStorage),
                            reinterpret_cast<const RequestHandler&>(rhsStorage));
        }
      } else if (http::IsMethodIdxSet(rhs._asyncMethodBmp, methodIdx)) {
        if (http::IsMethodIdxSet(_asyncMethodBmp, methodIdx)) {
          reinterpret_cast<AsyncRequestHandler&>(lhsStorage) = reinterpret_cast<const AsyncRequestHandler&>(rhsStorage);
        } else {
          std::construct_at(&reinterpret_cast<AsyncRequestHandler&>(lhsStorage),
                            reinterpret_cast<const AsyncRequestHandler&>(rhsStorage));
        }
      } else if (http::IsMethodIdxSet(rhs._streamingMethodBmp, methodIdx)) {
        if (http::IsMethodIdxSet(_streamingMethodBmp, methodIdx)) {
          reinterpret_cast<StreamingHandler&>(lhsStorage) = reinterpret_cast<const StreamingHandler&>(rhsStorage);
        } else {
          std::construct_at(&reinterpret_cast<StreamingHandler&>(lhsStorage),
                            reinterpret_cast<const StreamingHandler&>(rhsStorage));
        }
      } else {
        destroyIdx(methodIdx);
      }
    }

    _normalMethodBmp = rhs._normalMethodBmp;
    _streamingMethodBmp = rhs._streamingMethodBmp;
    _asyncMethodBmp = rhs._asyncMethodBmp;
  }
  return *this;
}

PathHandlerEntry& PathHandlerEntry::operator=(PathHandlerEntry&& rhs) noexcept {
  if (&rhs != this) [[likely]] {
#ifdef AERONET_ENABLE_WEBSOCKET
    _websocketEndpoint = std::move(rhs._websocketEndpoint);
#endif
    _corsPolicy = std::move(rhs._corsPolicy);
    _preMiddleware = std::move(rhs._preMiddleware);
    _postMiddleware = std::move(rhs._postMiddleware);
    _pathConfig = rhs._pathConfig;

    for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
      HandlerStorage& lhsStorage = _handlers[methodIdx];
      HandlerStorage& rhsStorage = rhs._handlers[methodIdx];

      if (http::IsMethodIdxSet(rhs._normalMethodBmp, methodIdx)) {
        if (http::IsMethodIdxSet(_normalMethodBmp, methodIdx)) {
          reinterpret_cast<RequestHandler&>(lhsStorage) = std::move(reinterpret_cast<RequestHandler&>(rhsStorage));
        } else {
          std::construct_at(&reinterpret_cast<RequestHandler&>(lhsStorage),
                            std::move(reinterpret_cast<RequestHandler&>(rhsStorage)));
        }
      } else if (http::IsMethodIdxSet(rhs._asyncMethodBmp, methodIdx)) {
        if (http::IsMethodIdxSet(_asyncMethodBmp, methodIdx)) {
          reinterpret_cast<AsyncRequestHandler&>(lhsStorage) =
              std::move(reinterpret_cast<AsyncRequestHandler&>(rhsStorage));
        } else {
          std::construct_at(&reinterpret_cast<AsyncRequestHandler&>(lhsStorage),
                            std::move(reinterpret_cast<AsyncRequestHandler&>(rhsStorage)));
        }
      } else if (http::IsMethodIdxSet(rhs._streamingMethodBmp, methodIdx)) {
        if (http::IsMethodIdxSet(_streamingMethodBmp, methodIdx)) {
          reinterpret_cast<StreamingHandler&>(lhsStorage) = std::move(reinterpret_cast<StreamingHandler&>(rhsStorage));
        } else {
          std::construct_at(&reinterpret_cast<StreamingHandler&>(lhsStorage),
                            std::move(reinterpret_cast<StreamingHandler&>(rhsStorage)));
        }
      } else {
        destroyIdx(methodIdx);
      }
    }

    _normalMethodBmp = std::exchange(rhs._normalMethodBmp, 0U);
    _streamingMethodBmp = std::exchange(rhs._streamingMethodBmp, 0U);
    _asyncMethodBmp = std::exchange(rhs._asyncMethodBmp, 0U);
  }
  return *this;
}

PathHandlerEntry::~PathHandlerEntry() {
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    destroyIdx(methodIdx);
  }
}

PathHandlerEntry& PathHandlerEntry::cors(CorsPolicy corsPolicy) {
  this->_corsPolicy = std::move(corsPolicy);
  return *this;
}

PathHandlerEntry& PathHandlerEntry::before(RequestMiddleware middleware) {
  _preMiddleware.emplace_back(std::move(middleware));
  return *this;
}

PathHandlerEntry& PathHandlerEntry::after(ResponseMiddleware middleware) {
  _postMiddleware.emplace_back(std::move(middleware));
  return *this;
}

void PathHandlerEntry::assignNormalHandler(http::MethodBmp methodBmp, RequestHandler handler) {
  const RequestHandler* pLastRequestHandler = nullptr;
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    if (!http::IsMethodIdxSet(methodBmp, methodIdx)) {
      continue;
    }
    HandlerStorage& storage = _handlers[methodIdx];
    http::MethodBmp localMethodBmp = http::MethodBmpFromIdx(methodIdx);

    if ((localMethodBmp & _normalMethodBmp) != 0) {
      if (pLastRequestHandler == nullptr) {
        // NOLINTNEXTLINE(bugprone-use-after-move)
        reinterpret_cast<RequestHandler&>(storage) = std::move(handler);
        pLastRequestHandler = &reinterpret_cast<RequestHandler&>(storage);
      } else {
        reinterpret_cast<RequestHandler&>(storage) = *pLastRequestHandler;
      }
    } else if ((localMethodBmp & _streamingMethodBmp) != 0) {
      throw std::logic_error("Cannot register normal handler: streaming handler already present for path+method");
    } else if ((localMethodBmp & _asyncMethodBmp) != 0) {
      throw std::logic_error("Cannot register normal handler: async handler already present for path+method");
    } else {
      _normalMethodBmp |= localMethodBmp;
      if (pLastRequestHandler == nullptr) {
        // NOLINTNEXTLINE(bugprone-use-after-move)
        std::construct_at(&reinterpret_cast<RequestHandler&>(storage), std::move(handler));
        pLastRequestHandler = &reinterpret_cast<RequestHandler&>(storage);
      } else {
        std::construct_at(&reinterpret_cast<RequestHandler&>(storage), *pLastRequestHandler);
      }
    }
  }
}

void PathHandlerEntry::assignAsyncHandler(http::MethodBmp methodBmp, AsyncRequestHandler handler) {
  const AsyncRequestHandler* pLastAsyncHandler = nullptr;
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    if (!http::IsMethodIdxSet(methodBmp, methodIdx)) {
      continue;
    }
    HandlerStorage& storage = _handlers[methodIdx];
    http::MethodBmp localMethodBmp = http::MethodBmpFromIdx(methodIdx);

    if ((localMethodBmp & _asyncMethodBmp) != 0) {
      if (pLastAsyncHandler == nullptr) {
        // NOLINTNEXTLINE(bugprone-use-after-move)
        reinterpret_cast<AsyncRequestHandler&>(storage) = std::move(handler);
        pLastAsyncHandler = &reinterpret_cast<AsyncRequestHandler&>(storage);
      } else {
        reinterpret_cast<AsyncRequestHandler&>(storage) = *pLastAsyncHandler;
      }
    } else if ((localMethodBmp & _normalMethodBmp) != 0) {
      throw std::logic_error("Cannot register async handler: normal handler already present for path+method");
    } else if ((localMethodBmp & _streamingMethodBmp) != 0) {
      throw std::logic_error("Cannot register async handler: streaming handler already present for path+method");
    } else {
      _asyncMethodBmp |= localMethodBmp;
      if (pLastAsyncHandler == nullptr) {
        // NOLINTNEXTLINE(bugprone-use-after-move)
        std::construct_at(&reinterpret_cast<AsyncRequestHandler&>(storage), std::move(handler));
        pLastAsyncHandler = &reinterpret_cast<AsyncRequestHandler&>(storage);
      } else {
        std::construct_at(&reinterpret_cast<AsyncRequestHandler&>(storage), *pLastAsyncHandler);
      }
    }
  }
}

void PathHandlerEntry::assignStreamingHandler(http::MethodBmp methodBmp, StreamingHandler handler) {
  const StreamingHandler* pLastStreamingHandler = nullptr;
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    if (!http::IsMethodIdxSet(methodBmp, methodIdx)) {
      continue;
    }
    HandlerStorage& storage = _handlers[methodIdx];
    http::MethodBmp localMethodBmp = http::MethodBmpFromIdx(methodIdx);

    if ((localMethodBmp & _streamingMethodBmp) != 0) {
      if (pLastStreamingHandler == nullptr) {
        // NOLINTNEXTLINE(bugprone-use-after-move)
        reinterpret_cast<StreamingHandler&>(storage) = std::move(handler);
        pLastStreamingHandler = &reinterpret_cast<StreamingHandler&>(storage);
      } else {
        reinterpret_cast<StreamingHandler&>(storage) = *pLastStreamingHandler;
      }
    } else if ((localMethodBmp & _normalMethodBmp) != 0) {
      throw std::logic_error("Cannot register streaming handler: normal handler already present for path+method");
    } else if ((localMethodBmp & _asyncMethodBmp) != 0) {
      throw std::logic_error("Cannot register streaming handler: async handler already present for path+method");
    } else {
      _streamingMethodBmp |= localMethodBmp;
      if (pLastStreamingHandler == nullptr) {
        // NOLINTNEXTLINE(bugprone-use-after-move)
        std::construct_at(&reinterpret_cast<StreamingHandler&>(storage), std::move(handler));
        pLastStreamingHandler = &reinterpret_cast<StreamingHandler&>(storage);
      } else {
        std::construct_at(&reinterpret_cast<StreamingHandler&>(storage), *pLastStreamingHandler);
      }
    }
  }
}

void PathHandlerEntry::destroyIdx(http::MethodIdx methodIdx) {
  HandlerStorage& storage = _handlers[methodIdx];

  if (http::IsMethodIdxSet(_normalMethodBmp, methodIdx)) {
    std::destroy_at(&reinterpret_cast<RequestHandler&>(storage));
  } else if (http::IsMethodIdxSet(_asyncMethodBmp, methodIdx)) {
    std::destroy_at(&reinterpret_cast<AsyncRequestHandler&>(storage));
  } else if (http::IsMethodIdxSet(_streamingMethodBmp, methodIdx)) {
    std::destroy_at(&reinterpret_cast<StreamingHandler&>(storage));
  }
}

#ifdef AERONET_ENABLE_WEBSOCKET
void PathHandlerEntry::assignWebSocketEndpoint(WebSocketEndpoint endpoint) {
  if (_websocketEndpoint) {
    *_websocketEndpoint = std::move(endpoint);
  } else {
    _websocketEndpoint = std::make_unique<WebSocketEndpoint>(std::move(endpoint));
  }
}
#endif

}  // namespace aeronet
