#include "aeronet/path-handler-entry.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/websocket-endpoint.hpp"

namespace aeronet {

PathHandlerEntry::PathHandlerEntry(const PathHandlerEntry& rhs)
    : normalMethodBmp(rhs.normalMethodBmp),
      streamingMethodBmp(rhs.streamingMethodBmp),
      asyncMethodBmp(rhs.asyncMethodBmp),
      websocketEndpoint(rhs.websocketEndpoint ? std::make_unique<WebSocketEndpoint>(*rhs.websocketEndpoint) : nullptr),
      corsPolicy(rhs.corsPolicy),
      preMiddleware(rhs.preMiddleware),
      postMiddleware(rhs.postMiddleware) {
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    const HandlerStorage& rhsStorage = rhs.handlers[methodIdx];
    HandlerStorage& lhsStorage = handlers[methodIdx];

    if (http::IsMethodIdxSet(normalMethodBmp, methodIdx)) {
      std::construct_at(&reinterpret_cast<RequestHandler&>(lhsStorage),
                        reinterpret_cast<const RequestHandler&>(rhsStorage));
    } else if (http::IsMethodIdxSet(asyncMethodBmp, methodIdx)) {
      std::construct_at(&reinterpret_cast<AsyncRequestHandler&>(lhsStorage),
                        reinterpret_cast<const AsyncRequestHandler&>(rhsStorage));
    } else if (http::IsMethodIdxSet(streamingMethodBmp, methodIdx)) {
      std::construct_at(&reinterpret_cast<StreamingHandler&>(lhsStorage),
                        reinterpret_cast<const StreamingHandler&>(rhsStorage));
    }
  }
}

PathHandlerEntry::PathHandlerEntry(PathHandlerEntry&& rhs) noexcept
    : websocketEndpoint(std::move(rhs.websocketEndpoint)),
      corsPolicy(std::move(rhs.corsPolicy)),
      preMiddleware(std::move(rhs.preMiddleware)),
      postMiddleware(std::move(rhs.postMiddleware)) {
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    HandlerStorage& rhsStorage = rhs.handlers[methodIdx];
    HandlerStorage& lhsStorage = handlers[methodIdx];

    if (http::IsMethodIdxSet(rhs.normalMethodBmp, methodIdx)) {
      std::construct_at(&reinterpret_cast<RequestHandler&>(lhsStorage),
                        std::move(reinterpret_cast<RequestHandler&>(rhsStorage)));
    } else if (http::IsMethodIdxSet(rhs.asyncMethodBmp, methodIdx)) {
      std::construct_at(&reinterpret_cast<AsyncRequestHandler&>(lhsStorage),
                        std::move(reinterpret_cast<AsyncRequestHandler&>(rhsStorage)));
    } else if (http::IsMethodIdxSet(rhs.streamingMethodBmp, methodIdx)) {
      std::construct_at(&reinterpret_cast<StreamingHandler&>(lhsStorage),
                        std::move(reinterpret_cast<StreamingHandler&>(rhsStorage)));
    }
  }

  normalMethodBmp = std::exchange(rhs.normalMethodBmp, 0U);
  streamingMethodBmp = std::exchange(rhs.streamingMethodBmp, 0U);
  asyncMethodBmp = std::exchange(rhs.asyncMethodBmp, 0U);
}

PathHandlerEntry& PathHandlerEntry::operator=(const PathHandlerEntry& rhs) {
  if (&rhs != this) {
    websocketEndpoint = rhs.websocketEndpoint ? std::make_unique<WebSocketEndpoint>(*rhs.websocketEndpoint) : nullptr;
    corsPolicy = rhs.corsPolicy;
    preMiddleware = rhs.preMiddleware;
    postMiddleware = rhs.postMiddleware;

    for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
      HandlerStorage& lhsStorage = handlers[methodIdx];
      const HandlerStorage& rhsStorage = rhs.handlers[methodIdx];

      if (http::IsMethodIdxSet(rhs.normalMethodBmp, methodIdx)) {
        if (http::IsMethodIdxSet(normalMethodBmp, methodIdx)) {
          reinterpret_cast<RequestHandler&>(lhsStorage) = reinterpret_cast<const RequestHandler&>(rhsStorage);
        } else {
          std::construct_at(&reinterpret_cast<RequestHandler&>(lhsStorage),
                            reinterpret_cast<const RequestHandler&>(rhsStorage));
        }
      } else if (http::IsMethodIdxSet(rhs.asyncMethodBmp, methodIdx)) {
        if (http::IsMethodIdxSet(asyncMethodBmp, methodIdx)) {
          reinterpret_cast<AsyncRequestHandler&>(lhsStorage) = reinterpret_cast<const AsyncRequestHandler&>(rhsStorage);
        } else {
          std::construct_at(&reinterpret_cast<AsyncRequestHandler&>(lhsStorage),
                            reinterpret_cast<const AsyncRequestHandler&>(rhsStorage));
        }
      } else if (http::IsMethodIdxSet(rhs.streamingMethodBmp, methodIdx)) {
        if (http::IsMethodIdxSet(streamingMethodBmp, methodIdx)) {
          reinterpret_cast<StreamingHandler&>(lhsStorage) = reinterpret_cast<const StreamingHandler&>(rhsStorage);
        } else {
          std::construct_at(&reinterpret_cast<StreamingHandler&>(lhsStorage),
                            reinterpret_cast<const StreamingHandler&>(rhsStorage));
        }
      } else {
        destroyIdx(methodIdx);
      }
    }

    normalMethodBmp = rhs.normalMethodBmp;
    streamingMethodBmp = rhs.streamingMethodBmp;
    asyncMethodBmp = rhs.asyncMethodBmp;
  }
  return *this;
}

PathHandlerEntry& PathHandlerEntry::operator=(PathHandlerEntry&& rhs) noexcept {
  if (&rhs != this) {
    websocketEndpoint = std::move(rhs.websocketEndpoint);
    corsPolicy = std::move(rhs.corsPolicy);
    preMiddleware = std::move(rhs.preMiddleware);
    postMiddleware = std::move(rhs.postMiddleware);

    for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
      HandlerStorage& lhsStorage = handlers[methodIdx];
      HandlerStorage& rhsStorage = rhs.handlers[methodIdx];

      if (http::IsMethodIdxSet(rhs.normalMethodBmp, methodIdx)) {
        if (http::IsMethodIdxSet(normalMethodBmp, methodIdx)) {
          reinterpret_cast<RequestHandler&>(lhsStorage) = std::move(reinterpret_cast<RequestHandler&>(rhsStorage));
        } else {
          std::construct_at(&reinterpret_cast<RequestHandler&>(lhsStorage),
                            std::move(reinterpret_cast<RequestHandler&>(rhsStorage)));
        }
      } else if (http::IsMethodIdxSet(rhs.asyncMethodBmp, methodIdx)) {
        if (http::IsMethodIdxSet(asyncMethodBmp, methodIdx)) {
          reinterpret_cast<AsyncRequestHandler&>(lhsStorage) =
              std::move(reinterpret_cast<AsyncRequestHandler&>(rhsStorage));
        } else {
          std::construct_at(&reinterpret_cast<AsyncRequestHandler&>(lhsStorage),
                            std::move(reinterpret_cast<AsyncRequestHandler&>(rhsStorage)));
        }
      } else if (http::IsMethodIdxSet(rhs.streamingMethodBmp, methodIdx)) {
        if (http::IsMethodIdxSet(streamingMethodBmp, methodIdx)) {
          reinterpret_cast<StreamingHandler&>(lhsStorage) = std::move(reinterpret_cast<StreamingHandler&>(rhsStorage));
        } else {
          std::construct_at(&reinterpret_cast<StreamingHandler&>(lhsStorage),
                            std::move(reinterpret_cast<StreamingHandler&>(rhsStorage)));
        }
      } else {
        destroyIdx(methodIdx);
      }
    }

    normalMethodBmp = std::exchange(rhs.normalMethodBmp, 0U);
    streamingMethodBmp = std::exchange(rhs.streamingMethodBmp, 0U);
    asyncMethodBmp = std::exchange(rhs.asyncMethodBmp, 0U);
  }
  return *this;
}

PathHandlerEntry::~PathHandlerEntry() {
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    destroyIdx(methodIdx);
  }
}

PathHandlerEntry& PathHandlerEntry::cors(CorsPolicy corsPolicy) {
  this->corsPolicy = std::move(corsPolicy);
  return *this;
}

PathHandlerEntry& PathHandlerEntry::before(RequestMiddleware middleware) {
  preMiddleware.emplace_back(std::move(middleware));
  return *this;
}

PathHandlerEntry& PathHandlerEntry::after(ResponseMiddleware middleware) {
  postMiddleware.emplace_back(std::move(middleware));
  return *this;
}

void PathHandlerEntry::assignNormalHandler(http::MethodBmp methodBmp, RequestHandler handler) {
  const RequestHandler* pLastRequestHandler = nullptr;
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    if (!http::IsMethodIdxSet(methodBmp, methodIdx)) {
      continue;
    }
    HandlerStorage& storage = handlers[methodIdx];
    http::MethodBmp localMethodBmp = http::MethodBmpFromIdx(methodIdx);

    if ((localMethodBmp & normalMethodBmp) != 0) {
      if (pLastRequestHandler == nullptr) {
        // NOLINTNEXTLINE(bugprone-use-after-move)
        reinterpret_cast<RequestHandler&>(storage) = std::move(handler);
        pLastRequestHandler = &reinterpret_cast<RequestHandler&>(storage);
      } else {
        reinterpret_cast<RequestHandler&>(storage) = *pLastRequestHandler;
      }
    } else if ((localMethodBmp & streamingMethodBmp) != 0) {
      throw std::logic_error("Cannot register normal handler: streaming handler already present for path+method");
    } else if ((localMethodBmp & asyncMethodBmp) != 0) {
      throw std::logic_error("Cannot register normal handler: async handler already present for path+method");
    } else {
      normalMethodBmp |= localMethodBmp;
      if (pLastRequestHandler == nullptr) {
        // NOLINTNEXTLINE(bugprone-use-after-move)
        std::construct_at(&reinterpret_cast<RequestHandler&>(storage), std::move(handler));
        pLastRequestHandler = &reinterpret_cast<RequestHandler&>(storage);
      } else {
        reinterpret_cast<RequestHandler&>(storage) = *pLastRequestHandler;
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
    HandlerStorage& storage = handlers[methodIdx];
    http::MethodBmp localMethodBmp = http::MethodBmpFromIdx(methodIdx);

    if ((localMethodBmp & asyncMethodBmp) != 0) {
      if (pLastAsyncHandler == nullptr) {
        // NOLINTNEXTLINE(bugprone-use-after-move)
        reinterpret_cast<AsyncRequestHandler&>(storage) = std::move(handler);
        pLastAsyncHandler = &reinterpret_cast<AsyncRequestHandler&>(storage);
      } else {
        reinterpret_cast<AsyncRequestHandler&>(storage) = *pLastAsyncHandler;
      }
    } else if ((localMethodBmp & normalMethodBmp) != 0) {
      throw std::logic_error("Cannot register async handler: normal handler already present for path+method");
    } else if ((localMethodBmp & streamingMethodBmp) != 0) {
      throw std::logic_error("Cannot register async handler: streaming handler already present for path+method");
    } else {
      asyncMethodBmp |= localMethodBmp;
      if (pLastAsyncHandler == nullptr) {
        // NOLINTNEXTLINE(bugprone-use-after-move)
        std::construct_at(&reinterpret_cast<AsyncRequestHandler&>(storage), std::move(handler));
        pLastAsyncHandler = &reinterpret_cast<AsyncRequestHandler&>(storage);
      } else {
        reinterpret_cast<AsyncRequestHandler&>(storage) = *pLastAsyncHandler;
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
    HandlerStorage& storage = handlers[methodIdx];
    http::MethodBmp localMethodBmp = http::MethodBmpFromIdx(methodIdx);

    if ((localMethodBmp & streamingMethodBmp) != 0) {
      if (pLastStreamingHandler == nullptr) {
        // NOLINTNEXTLINE(bugprone-use-after-move)
        reinterpret_cast<StreamingHandler&>(storage) = std::move(handler);
        pLastStreamingHandler = &reinterpret_cast<StreamingHandler&>(storage);
      } else {
        reinterpret_cast<StreamingHandler&>(storage) = *pLastStreamingHandler;
      }
    } else if ((localMethodBmp & normalMethodBmp) != 0) {
      throw std::logic_error("Cannot register streaming handler: normal handler already present for path+method");
    } else if ((localMethodBmp & asyncMethodBmp) != 0) {
      throw std::logic_error("Cannot register streaming handler: async handler already present for path+method");
    } else {
      streamingMethodBmp |= localMethodBmp;
      if (pLastStreamingHandler == nullptr) {
        // NOLINTNEXTLINE(bugprone-use-after-move)
        std::construct_at(&reinterpret_cast<StreamingHandler&>(storage), std::move(handler));
        pLastStreamingHandler = &reinterpret_cast<StreamingHandler&>(storage);
      } else {
        reinterpret_cast<StreamingHandler&>(storage) = *pLastStreamingHandler;
      }
    }
  }
}

void PathHandlerEntry::destroyIdx(http::MethodIdx methodIdx) {
  HandlerStorage& storage = handlers[methodIdx];

  if (http::IsMethodIdxSet(normalMethodBmp, methodIdx)) {
    std::destroy_at(&reinterpret_cast<RequestHandler&>(storage));
  } else if (http::IsMethodIdxSet(asyncMethodBmp, methodIdx)) {
    std::destroy_at(&reinterpret_cast<AsyncRequestHandler&>(storage));
  } else if (http::IsMethodIdxSet(streamingMethodBmp, methodIdx)) {
    std::destroy_at(&reinterpret_cast<StreamingHandler&>(storage));
  }
}

void PathHandlerEntry::assignWebSocketEndpoint(WebSocketEndpoint endpoint) {
  if (websocketEndpoint) {
    *websocketEndpoint = std::move(endpoint);
  } else {
    websocketEndpoint = std::make_unique<WebSocketEndpoint>(std::move(endpoint));
  }
}

}  // namespace aeronet