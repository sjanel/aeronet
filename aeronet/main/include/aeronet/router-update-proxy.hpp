#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/path-handler-entry.hpp"
#include "aeronet/path-handlers.hpp"
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
 public:
  using Dispatcher = std::function<void(std::function<void(Router&)>)>;

  /** Replace the entire router on the server thread. See Router::operator=(Router&&) */
  RouterUpdateProxy& operator=(Router router) {
    (*_dispatcher)([router = std::move(router)](Router& target) mutable { target = std::move(router); });
    return *this;
  }

  /** Clear all handlers and middleware. See Router::clear() */
  RouterUpdateProxy& clear() {
    (*_dispatcher)([](Router& router) { router.clear(); });
    return *this;
  }

  /** Set the default fixed-response handler. See Router::setDefault(RequestHandler) */
  RouterUpdateProxy& setDefault(RequestHandler handler) {
    (*_dispatcher)([handler = std::move(handler)](Router& router) mutable { router.setDefault(std::move(handler)); });
    return *this;
  }

  /** Set the default streaming handler. See Router::setDefault(StreamingHandler) */
  RouterUpdateProxy& setDefault(StreamingHandler handler) {
    (*_dispatcher)([handler = std::move(handler)](Router& router) mutable { router.setDefault(std::move(handler)); });
    return *this;
  }

  /** Add a global request middleware. See Router::addRequestMiddleware */
  RouterUpdateProxy& addRequestMiddleware(RequestMiddleware middleware) {
    (*_dispatcher)([middleware = std::move(middleware)](Router& router) mutable {
      router.addRequestMiddleware(std::move(middleware));
    });
    return *this;
  }

  /** Add a global response middleware. See Router::addResponseMiddleware */
  RouterUpdateProxy& addResponseMiddleware(ResponseMiddleware middleware) {
    (*_dispatcher)([middleware = std::move(middleware)](Router& router) mutable {
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
    PathEntryProxy(std::shared_ptr<Dispatcher> dispatcher, std::shared_ptr<PathHandlerEntry*> entryPtr)
        : _dispatcher(std::move(dispatcher)), _entryPtr(std::move(entryPtr)) {}

    /** Install per-path request middleware. See PathHandlerEntry::before */
    PathEntryProxy& before(RequestMiddleware middleware) {
      (*_dispatcher)([entryPtr = _entryPtr, middleware = std::move(middleware)](Router&) mutable {
        if (auto* entry = *entryPtr) {
          entry->before(std::move(middleware));
        }
      });
      return *this;
    }

    /** Install per-path response middleware. See PathHandlerEntry::after */
    PathEntryProxy& after(ResponseMiddleware middleware) {
      (*_dispatcher)([entryPtr = _entryPtr, middleware = std::move(middleware)](Router&) mutable {
        if (auto* entry = *entryPtr) {
          entry->after(std::move(middleware));
        }
      });
      return *this;
    }

    /** Set CORS policy for the registered path. See PathHandlerEntry::cors */
    PathEntryProxy& cors(CorsPolicy policy) {
      (*_dispatcher)([entryPtr = _entryPtr, policy = std::move(policy)](Router&) mutable {
        if (auto* entry = *entryPtr) {
          entry->cors(std::move(policy));
        }
      });
      return *this;
    }

   private:
    std::shared_ptr<Dispatcher> _dispatcher;
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
  PathEntryProxy setPath(http::Method method, std::string_view path, AsyncRequestHandler handler) {
    return setPathImpl(method, path, std::move(handler));
  }

  /**
   * Register an async handler for a bitmap of HTTP methods and a path.
   * This operation is dispatched to the server event-loop and applied there.
   * Returns a PathEntryProxy for further per-path configuration.
   *
   * See also: Router::setPath(http::MethodBmp, std::string_view, AsyncRequestHandler)
   */
  PathEntryProxy setPath(http::MethodBmp methods, std::string_view path, AsyncRequestHandler handler) {
    return setPathImpl(methods, path, std::move(handler));
  }

 private:
  friend class SingleHttpServer;
  friend class MultiHttpServer;

  /**
   * Construct a RouterUpdateProxy.
   *
   * @param dispatcher A callable used to post updater callbacks to the server's event loop.
   * @param directAccessor A zero-argument function returning a direct `Router&` for pre-start
   *        inspection. Calling the direct accessor while the server is running may throw.
   */
  RouterUpdateProxy(Dispatcher dispatcher, std::function<Router&()> directAccessor)
      : _dispatcher(std::make_shared<Dispatcher>(std::move(dispatcher))), _direct(std::move(directAccessor)) {}

  /**
   * Internal helper that posts a setPath call to the dispatcher and returns
   * a PathEntryProxy pointing to the registered entry. The method parameter
   * is forwarded to Router::setPath.
   */
  template <typename MethodTag, typename Handler>
  PathEntryProxy setPathImpl(MethodTag method, std::string_view path, Handler handler) {
    auto entryPtr = std::make_shared<PathHandlerEntry*>(nullptr);
    auto dispatcher = _dispatcher;
    // Make an owning copy of `path` for the posted callback. The dispatcher may
    // execute the lambda asynchronously on the server thread, so a
    // std::string_view would dangle if the caller's storage is destroyed.
    (*dispatcher)([entryPtr, method, path = std::string(path), handler = std::move(handler)](Router& router) mutable {
      auto& entry = router.setPath(method, path, std::move(handler));
      *entryPtr = &entry;
    });
    return {std::move(dispatcher), std::move(entryPtr)};
  }

  std::shared_ptr<Dispatcher> _dispatcher;
  std::function<Router&()> _direct;
};

}  // namespace aeronet
