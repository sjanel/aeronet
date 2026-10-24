#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/path-handler-entry.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"

namespace aeronet {

/**
 * RouterUpdateProxy
 *
 * A small proxy object that lets callers post router-updating callbacks to the server's
 * event-loop dispatcher. It provides a fluent API mirroring parts of `Router` so callers
 * can register handlers or middleware from other threads safely. For full semantics of
 * each operation refer to the corresponding `Router::...` method linked in each member's
 * documentation.
 */
class RouterUpdateProxy {
  // Posts router updaters to the event loop of the server owning the router.
  struct Dispatcher {
    void operator()(std::function<void(Router&)> updater) const { pDispatch(pServer, std::move(updater)); }

    void (*pDispatch)(void* pServer, std::function<void(Router&)> updater);
    void* pServer;
  };

 public:
  /** Replace the entire router on the server thread. See Router::operator=(Router&&) */
  RouterUpdateProxy& operator=(Router router) {
    _dispatcher([router = std::move(router)](Router& target) mutable { target = std::move(router); });
    return *this;
  }

  /** Clear all handlers and middleware. See Router::clear() */
  RouterUpdateProxy& clear() {
    _dispatcher([](Router& router) { router.clear(); });
    return *this;
  }

  /** Set the default fixed-response handler. See Router::setDefault(RequestHandler) */
  RouterUpdateProxy& setDefault(RequestHandler handler) {
    _dispatcher([handler = std::move(handler)](Router& router) mutable { router.setDefault(std::move(handler)); });
    return *this;
  }

  /** Set the default streaming handler. See Router::setDefault(StreamingHandler) */
  RouterUpdateProxy& setDefault(StreamingHandler handler) {
    _dispatcher([handler = std::move(handler)](Router& router) mutable { router.setDefault(std::move(handler)); });
    return *this;
  }

  /** Set the default asynchronous handler. See Router::setDefault(AsyncRequestHandler) */
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  RouterUpdateProxy& setDefault(AsyncRequestHandler handler) {
    _dispatcher([handler = std::move(handler)](Router& router) mutable { router.setDefault(std::move(handler)); });
    return *this;
  }
#endif

  /** Add a global request middleware. See Router::addRequestMiddleware */
  RouterUpdateProxy& addRequestMiddleware(RequestMiddleware middleware) {
    _dispatcher([middleware = std::move(middleware)](Router& router) mutable {
      router.addRequestMiddleware(std::move(middleware));
    });
    return *this;
  }

  /** Add a global response middleware. See Router::addResponseMiddleware */
  RouterUpdateProxy& addResponseMiddleware(ResponseMiddleware middleware) {
    _dispatcher([middleware = std::move(middleware)](Router& router) mutable {
      router.addResponseMiddleware(std::move(middleware));
    });
    return *this;
  }

  /**
   * PathEntryProxy provides a handle to further configure the registered path entry
   * (per-path middleware, CORS policy). All operations are dispatched to the server thread.
   */
  class PathEntryProxy {
   public:
    PathEntryProxy(Dispatcher dispatcher, std::shared_ptr<PathHandlerEntry*> entryPtr) noexcept
        : _dispatcher(dispatcher), _entryPtr(std::move(entryPtr)) {}

    /** Install per-path request middleware. See PathHandlerEntry::before */
    PathEntryProxy& before(RequestMiddleware middleware) {
      _dispatcher([entryPtr = _entryPtr, middleware = std::move(middleware)](Router&) mutable {
        if (auto* entry = *entryPtr) {
          entry->before(std::move(middleware));
        }
      });
      return *this;
    }

    /** Install per-path response middleware. See PathHandlerEntry::after */
    PathEntryProxy& after(ResponseMiddleware middleware) {
      _dispatcher([entryPtr = _entryPtr, middleware = std::move(middleware)](Router&) mutable {
        if (auto* entry = *entryPtr) {
          entry->after(std::move(middleware));
        }
      });
      return *this;
    }

    /** Set CORS policy for the registered path. See PathHandlerEntry::cors */
    PathEntryProxy& cors(CorsPolicy policy) {
      _dispatcher([entryPtr = _entryPtr, policy = std::move(policy)](Router&) mutable {
        if (auto* entry = *entryPtr) {
          entry->cors(std::move(policy));
        }
      });
      return *this;
    }

#ifdef AERONET_ENABLE_HTTP2
    /** Set HTTP/2 enable mode for the registered path. See PathHandlerEntry::http2Enable */
    PathEntryProxy& http2Enable(PathEntryConfig::Http2Enable mode) {
      _dispatcher([entryPtr = _entryPtr, mode](Router&) {
        if (auto* entry = *entryPtr) {
          entry->http2Enable(mode);
        }
      });
      return *this;
    }
#endif

    /** Set per-route maximum header size. See PathHandlerEntry::maxHeaderBytes */
    PathEntryProxy& maxHeaderBytes(uint32_t bytes) {
      _dispatcher([entryPtr = _entryPtr, bytes](Router&) {
        if (auto* entry = *entryPtr) {
          entry->maxHeaderBytes(bytes);
        }
      });
      return *this;
    }

    /** Set per-route maximum body size. See PathHandlerEntry::maxBodyBytes */
    PathEntryProxy& maxBodyBytes(std::size_t bytes) {
      _dispatcher([entryPtr = _entryPtr, bytes](Router&) {
        if (auto* entry = *entryPtr) {
          entry->maxBodyBytes(bytes);
        }
      });
      return *this;
    }

    /** Set per-route handler deadline. See PathHandlerEntry::timeout */
    PathEntryProxy& timeout(std::chrono::milliseconds ms) {
      _dispatcher([entryPtr = _entryPtr, ms](Router&) {
        if (auto* entry = *entryPtr) {
          entry->timeout(ms);
        }
      });
      return *this;
    }

   private:
    Dispatcher _dispatcher;
    std::shared_ptr<PathHandlerEntry*> _entryPtr;
  };

  /**
   * Register a fixed (non-streaming) handler for a single HTTP method and path.
   * This operation is dispatched to the server event-loop and applied there.
   * Returns a PathEntryProxy for further per-path configuration.
   *
   * See also: Router::setPath(http::Method, std::string_view, Router::RequestHandler)
   */
  PathEntryProxy setPath(http::Method method, std::string_view path, RequestHandler handler) {
    return setPathImpl(method, path, std::move(handler));
  }

  /**
   * Register a fixed (non-streaming) handler for a bitmap of methods and a path.
   * This operation is dispatched to the server event-loop and applied there.
   * Returns a PathEntryProxy for further per-path configuration.
   *
   * See also: Router::setPath(http::MethodBmp, std::string_view, RequestHandler)
   */
  PathEntryProxy setPath(http::MethodBmp methods, std::string_view path, RequestHandler handler) {
    return setPathImpl(methods, path, std::move(handler));
  }

  /**
   * Register a streaming handler for a single HTTP method and path.
   * This operation is dispatched to the server event-loop and applied there.
   * Returns a PathEntryProxy for further per-path configuration.
   *
   * See also: Router::setPath(http::Method, std::string_view, StreamingHandler)
   */
  PathEntryProxy setPath(http::Method method, std::string_view path, StreamingHandler handler) {
    return setPathImpl(method, path, std::move(handler));
  }

  /**
   * Register a streaming handler for a bitmap of methods and a path.
   * This operation is dispatched to the server event-loop and applied there.
   * Returns a PathEntryProxy for further per-path configuration.
   *
   * See also: Router::setPath(http::MethodBmp, std::string_view, StreamingHandler)
   */
  PathEntryProxy setPath(http::MethodBmp methods, std::string_view path, StreamingHandler handler) {
    return setPathImpl(methods, path, std::move(handler));
  }

  /**
   * Register an async handler for a single HTTP method and path.
   * This operation is dispatched to the server event-loop and applied there.
   * Returns a PathEntryProxy for further per-path configuration.
   *
   * See also: Router::setPath(http::Method, std::string_view, AsyncRequestHandler)
   */
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  PathEntryProxy setPath(http::Method method, std::string_view path, AsyncRequestHandler handler) {
    return setPathImpl(method, path, std::move(handler));
  }
#endif

  /**
   * Register an async handler for a bitmap of HTTP methods and a path.
   * This operation is dispatched to the server event-loop and applied there.
   * Returns a PathEntryProxy for further per-path configuration.
   *
   * See also: Router::setPath(http::MethodBmp, std::string_view, AsyncRequestHandler)
   */
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  PathEntryProxy setPath(http::MethodBmp methods, std::string_view path, AsyncRequestHandler handler) {
    return setPathImpl(methods, path, std::move(handler));
  }
#endif

 private:
  friend class SingleHttpServer;
  friend class MultiHttpServer;

  /**
   * Construct a RouterUpdateProxy.
   *
   * @param pDispatch Posts an updater callback to the event loop of the server 'pServer'.
   * @param pServer The server owning the router.
   */
  RouterUpdateProxy(void (*pDispatch)(void* pServer, std::function<void(Router&)> updater), void* pServer) noexcept
      : _dispatcher{pDispatch, pServer} {}

  /**
   * Internal helper that posts a setPath call to the dispatcher and returns
   * a PathEntryProxy pointing to the registered entry. The method parameter
   * is forwarded to Router::setPath.
   */
  template <typename MethodTag, typename Handler>
  PathEntryProxy setPathImpl(MethodTag method, std::string_view path, Handler handler) {
    auto entryPtr = std::make_shared<PathHandlerEntry*>(nullptr);
    // Make an owning copy of `path` for the posted callback. The dispatcher may
    // execute the lambda asynchronously on the server thread, so a
    // std::string_view would dangle if the caller's storage is destroyed.
    _dispatcher([entryPtr, method, path = RawChars32(path), handler = std::move(handler)](Router& router) mutable {
      auto& entry = router.setPath(method, path, std::move(handler));
      *entryPtr = &entry;
    });
    return {_dispatcher, std::move(entryPtr)};
  }

  Dispatcher _dispatcher;
};

}  // namespace aeronet
