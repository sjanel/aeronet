#pragma once

#include <array>
#include <climits>
#include <cstdint>
#include <functional>
#include <string>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/router-config.hpp"
#include "flat-hash-map.hpp"

namespace aeronet {

class Router {
 public:
  using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

  using StreamingHandler = std::function<void(const HttpRequest&, HttpResponseWriter&)>;

  // Creates an empty Router with a 'Normalize' trailing policy.
  Router() noexcept = default;

  // Creates an empty Router with the configuration taken from object.
  explicit Router(RouterConfig config);

  // Registers a single request handler that will be invoked for every successfully parsed
  // HTTP request not matched by a path‑specific handler (normal or streaming). The handler
  // receives a fully populated immutable HttpRequest reference and must return an HttpResponse
  // by value (moved out). The returned response is serialized and queued for write immediately
  // after the handler returns.
  //
  // Precedence (Phase 2 mixing model):
  //   1. Path streaming handler (if registered for path+method)
  //   2. Path normal handler (if registered for path+method)
  //   3. Global streaming handler (if set)
  //   4. Global normal handler (this)
  //   5. 404 / 405 fallback
  //
  // Mixing:
  //   - Global normal and streaming handlers may both be set; per‑path handlers override them.
  //   - Replacing a global handler is allowed at any time (not thread‑safe; caller must ensure exclusive access).
  //
  // Timing & threading:
  //   - The handler executes synchronously inside the server's single event loop thread; do
  //     not perform blocking operations of long duration inside it (offload to another thread
  //     if needed and respond later via a queued response mechanism – future enhancement).
  //   - Because only one event loop thread exists per server instance, no additional
  //     synchronization is required for data local to the handler closure, but you must still
  //     synchronize access to state shared across multiple server instances.
  //
  // Lifetime:
  //   - You may call setDefault() before or after run()/runUntil(); replacing the handler
  //     while the server is processing requests is safe (the new handler will be used for
  //     subsequent requests) but avoid doing so concurrently from another thread.
  //
  // Error handling:
  //   - Exceptions escaping the handler will be caught, converted to a 500 response, and the
  //     connection may be closed depending on the internal policy (implementation detail).
  //
  // Performance notes:
  //   - Returning large payloads benefits from move semantics; construct HttpResponse in place
  //     and return; small-string optimizations usually avoid allocations for short headers.
  void setDefault(RequestHandler handler);

  // Enables incremental / chunked style responses using HttpResponseWriter instead of returning
  // a fully materialized HttpResponse object. Intended for large / dynamic payloads (server‑sent
  // data, on‑the‑fly generation) or when you wish to start sending bytes before the complete
  // body is available.
  //
  // Mixing (Phase 2):
  //   - May coexist with a global normal handler and with per‑path (normal or streaming) handlers.
  //   - Acts only as a fallback when no path‑specific handler matches.
  //
  // Invocation semantics:
  //   - The streaming handler runs synchronously inside the event loop thread after a request
  //     has been fully parsed (headers + body by current design). Future evolution may allow
  //     body streaming; today you receive the complete request body.
  //   - For HEAD requests the writer is constructed in a mode that suppresses body emission; you
  //     may still call write() but payload bytes are discarded while headers are sent.
  //
  // Writer contract:
  //   - You may set status / headers up until the first write(). If you never explicitly set a
  //     Content-Length the response is transferred using chunked encoding (unless HTTP/1.0).
  //   - Call writer.end() to finalize the response. If you return without calling end(), the
  //     server will automatically end() for you (sending last chunk / final CRLF) unless a fatal
  //     error occurred.
  //   - write() applies simple backpressure by queuing into the connection's outbound buffer; a
  //     false return indicates a fatal condition (connection closing / overflow) – cease writing.
  //
  // Keep‑alive & connection reuse:
  //   - After the handler returns the server evaluates standard keep‑alive rules (HTTP/1.1,
  //     config.enableKeepAlive, request count < maxRequestsPerConnection, no close flag). If any
  //     condition fails the connection is marked to close once buffered bytes flush.
  //
  // Performance & blocking guidance:
  //   - Avoid long blocking operations; they stall the entire server instance. Offload heavy
  //     work to a different thread and stream results back if necessary (future async hooks may
  //     simplify this pattern).
  //
  // Exceptions:
  //   - Exceptions thrown by the handler are caught and logged; the server attempts to end the
  //     response gracefully (typically as already started chunked stream). Subsequent writes are
  //     ignored once a failure state is reached.
  void setDefault(StreamingHandler handler);

  // Register a handler for a specific absolute path and a set of allowed HTTP methods.
  // May coexist with global handlers and with per-path streaming handlers (but a specific
  // (path, method) pair cannot have both a normal and streaming handler simultaneously).
  // Methods can be combined using bitwise OR (e.g. http::Method::GET | http::Method::POST).
  void setPath(http::MethodBmp methods, std::string path, RequestHandler handler);

  // Register a handler for a specific absolute path and a unique allowed HTTP methods.
  // May coexist with global handlers and with per-path streaming handlers (but a specific
  // (path, method) pair cannot have both a normal and streaming handler simultaneously).
  void setPath(http::Method method, std::string path, RequestHandler handler);

  // Registers streaming handlers per path+method combination. Mirrors setPath semantics
  // but installs a StreamingHandler which receives an HttpResponseWriter.
  // Methods can be combined using bitwise OR (e.g. http::Method::GET | http::Method::POST).
  // Constraints:
  //   - For each (path, method) only one of normal vs streaming may be present; registering the
  //     other kind afterwards is a logic error.
  // Overwrite semantics:
  //   - Re-registering the same kind (streaming over streaming) replaces the previous handler.
  void setPath(http::MethodBmp methods, std::string path, StreamingHandler handler);

  // Registers streaming handlers per path+method combination. Mirrors setPath semantics
  // but installs a StreamingHandler which receives an HttpResponseWriter.
  // Constraints:
  //   - For each (path, method) only one of normal vs streaming may be present; registering the
  //     other kind afterwards is a logic error.
  // Overwrite semantics:
  //   - Re-registering the same kind (streaming over streaming) replaces the previous handler.
  void setPath(http::Method method, std::string path, StreamingHandler handler);

  struct RoutingResult {
    // Only one of them will be non null if found
    const RequestHandler* requestHandler{nullptr};
    const StreamingHandler* streamingHandler{nullptr};
    enum class RedirectSlashMode : int8_t {
      None,        // Indicates that no redirection is needed
      AddSlash,    // Indicates that a redirection to add a trailing slash is needed
      RemoveSlash  // Indicates that a redirection to remove a trailing slash is needed
    } redirectPathIndicator{RedirectSlashMode::None};
    bool methodNotAllowed{false};
  };

  // Query the router for a matching handler for the given method and path.
  // The object returned contains pointers to the matched handler (if any),
  // a redirect indicator (if applicable), and a methodNotAllowed flag.
  // Other information may be added in the future.
  // Note: the returned pointers are valid as long as the Router instance
  // is not modified (no handler registration or replacement).
  [[nodiscard]] RoutingResult match(http::Method method, std::string_view path) const;

  // Return a bitmap of methods allowed for the given path. For a specific registered path
  // this returns the union of normal and streaming method bitmaps. When the empty result
  // is returned it indicates no handlers (nor global handlers) apply for that path. If
  // the Router has global handlers installed, those are treated as allowing all methods.
  [[nodiscard]] http::MethodBmp allowedMethods(std::string_view path) const;

 private:
  struct PathHandlerEntry {
    http::MethodBmp normalMethodBmp{};
    http::MethodBmp streamingMethodBmp{};
    bool isNormalized{false};
    std::array<RequestHandler, http::kNbMethods> normalHandlers{};
    std::array<StreamingHandler, http::kNbMethods> streamingHandlers{};
  };

  RequestHandler _handler;
  StreamingHandler _streamingHandler;
  flat_hash_map<std::string, PathHandlerEntry, std::hash<std::string_view>, std::equal_to<>> _pathHandlers;

  RouterConfig _config;
};

}  // namespace aeronet