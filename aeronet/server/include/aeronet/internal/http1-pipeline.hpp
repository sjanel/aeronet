#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aeronet/compiler-config.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/router.hpp"
#include "aeronet/sv-to-sv-map.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/async-handler-state.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handlers.hpp"
#endif

namespace aeronet {

class CorsPolicy;

namespace internal {

struct ServerContext;

// The HTTP/1.1 side of a SingleHttpServer: the framing of the requests (head, fixed-length and chunked bodies with
// their trailers, Expect handling, request body decompression), the protocol-specific answers (errors, HTTPS redirect,
// OPTIONS / TRACE / CONNECT, WebSocket upgrade), the call of the handlers through the request pipeline shared with
// HTTP/2 (streaming and async handlers included), and the framing of the responses.
class Http1Pipeline {
 public:
  using ConnectionIt = ConnectionStorage::ConnectionIt;

  // What the request loop does after a step of the handling of a request.
  enum class LoopAction : uint8_t { Nothing, Continue, Break, SwitchProtocol };

  explicit Http1Pipeline(ServerContext& ctx) noexcept : _pCtx(&ctx) {}

  // Points to the context of the server owning this component, after a move of that context.
  void bind(ServerContext& ctx) noexcept { _pCtx = &ctx; }

  // Serves the HTTP/1.1 requests in the input buffer of the connection, then writes their responses not written yet.
  // The responses to the requests followed by others are batched (see ConnectionIo::batchOutputOf()). The bytes
  // following an upgrade request are left in the input buffer, for the new protocol.
  // Returns true if the connection should stop reading (or close).
  bool processRequests(ConnectionIt cnxIt);

  // Whether the connection of this HTTP/1.x request stays open after its response: false when the client asked to close
  // it, keep-alive is disabled, the request is the last one allowed on the connection, or the server drains or stops.
  // The response then announces the close (RFC 9112 §9.6).
  [[nodiscard]] bool isKeepAlive(const HttpRequestView& request) const;

  // Sends the response to the current HTTP/1.1 request, applying the CORS policy of its route (if any). With 'close',
  // the connection closes after it, as when the request does not keep it alive.
  void finalizeAndSendResponse(ConnectionIt cnxIt, HttpResponse&& resp, const CorsPolicy* pCorsPolicy,
                               bool close = false);

  // Builds & queues a simple error response, invoking the parser error callback (if any). The response to a HEAD
  // request has the header fields of the GET one, without its body.
  // The connection will be closed after draining buffered writes.
  void emitSimpleError(ConnectionIt cnxIt, http::StatusCode statusCode, std::string_view body = {});

  // End an HTTP/1.1 request answered with 'status': emit its metrics (if observed), then end its span.
  void endRequest(HttpRequestView& request, http::StatusCode status);

  // Answers the request whose head is still being received after a read when its input exceeds maxHeaderBytes (414 if
  // the end of its request line was not received, 431 otherwise) or its headerReadTimeout elapsed (408). Returns true
  // if it was answered: the connection then closes.
  bool rejectIncompleteHead(ConnectionIt cnxIt);

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Tells whether the given HTTP/1 connection must stop reading because of its async handler (and records it).
  static bool PauseReadingDuringAsyncHandler(ConnectionState& state) noexcept;

  // Resumes the coroutine of the async handler of the connection, then sends its response if it completed.
  void resumeAsyncHandler(ConnectionIt cnxIt);
#endif

 private:
  enum class BodyDecodeStatus : uint8_t { Ready, NeedMore, Error };

  // The loop of processRequests().
  bool serveRequests(ConnectionIt cnxIt);

  // The steps of serveRequests() on the current request of the connection (state), inlined into it (they run for each
  // request, and are only called there).

  // Parses the head of the current request starting 'input' (or resumes the one received before its body), then runs
  // the checks that do not depend on its route: HTTPS redirect, Transfer-Encoding (sets isChunked), Accept-Encoding.
  // Returns Nothing when the request can be routed, Continue when it was answered from its head (406), its head being
  // left to consume, and Break otherwise.
  AERONET_ALWAYS_INLINE LoopAction readHead(ConnectionIt cnxIt, ConnectionState& state, std::span<char> input,
                                            bool& isChunked);

  // Arms the per-route request deadline of the current request, and checks its head against the per-route head size
  // limit. Returns false if it is answered by an error.
  AERONET_ALWAYS_INLINE bool applyRouteLimits(ConnectionIt cnxIt, ConnectionState& state,
                                              const Router::RoutingResult& routingResult);

  // Answers the current request, routed and with its special methods handled: runs its request middleware (unless it
  // ran before 100 Continue was sent), then its WebSocket upgrade or its handler. Returns Nothing when its async
  // handler is to be dispatched.
  AERONET_ALWAYS_INLINE LoopAction callHandlers(ConnectionIt cnxIt, ConnectionState& state,
                                                const Router::RoutingResult& routingResult, bool bodyReady);

  // The client of the current request, routed, waits for 100 Continue before sending its body (RFC 9110 §10.1.1), that
  // its handler needs. Answers it right away when its final status is known without its body: a special method, a
  // request middleware answering, a CORS denial, or no handler for its method and path (404, 405, trailing slash
  // redirect). The connection then closes, its body not being read. Otherwise, sends 100 Continue, its request
  // middleware having run: its handler is called once its body is received.
  // Returns Nothing when 100 Continue was sent, Continue or Break (as callHandlers()) when it was answered.
  LoopAction answerBeforeBody(ConnectionIt cnxIt, ConnectionState& state, const Router::RoutingResult& routingResult);

  // Validate the Transfer-Encoding of the current request. Returns true for chunked, the only coding supported.
  // Otherwise, queues the error response (the connection then closes) and returns false.
  bool acceptChunkedTransferEncoding(ConnectionIt cnxIt, std::string_view transferEncoding);

  // Apply the response middleware of the route, then send the response to the current request. With 'close', the
  // connection closes after it.
  void sendResponse(ConnectionIt cnxIt, HttpResponse&& resp, const Router::RoutingResult& routingResult, bool streaming,
                    bool close = false);

  // Run the streaming handler of the current request. Returns Break when the connection closes afterwards, Continue
  // otherwise.
  LoopAction serveStreamingRequest(ConnectionIt cnxIt, const Router::RoutingResult& routingResult);

  // Decodes the body of the current request, whose head starts 'input' (the input buffer from the request on).
  // Sets consumedBytes to the bytes of the request (head, body and trailers) once Ready.
  BodyDecodeStatus decodeBodyIfReady(ConnectionIt cnxIt, std::span<char> input, bool isChunked,
                                     std::size_t maxBodyBytes, std::size_t& consumedBytes);
  BodyDecodeStatus decodeFixedLengthBody(ConnectionIt cnxIt, std::span<const char> input, std::size_t maxBodyBytes,
                                         std::size_t& consumedBytes);
  // The body of a chunked request received in several reads is decoded once: the progress of its decoding is kept in
  // the connection (see ConnectionState::ChunkedBodyProgress), each read decoding the chunks received since the
  // previous one.
  BodyDecodeStatus decodeChunkedBody(ConnectionIt cnxIt, std::span<char> input, std::size_t maxBodyBytes,
                                     std::size_t& consumedBytes);

  // Decodes the chunks of the body of the current request received since its last decoding, up to its end (trailers
  // included). Keeps its progress in the connection if the body is incomplete (NeedMore), whose caller resets it
  // otherwise.
  BodyDecodeStatus decodeNextChunks(ConnectionIt cnxIt, std::span<char> input, std::size_t maxBodyBytes,
                                    std::size_t& consumedBytes);

  // Decodes the trailers of a chunked body, starting at pos in 'input' (after the last chunk): copies them to the body
  // and trailers buffer of the connection, and parses them. Sets pos after the blank line ending them once Ready.
  BodyDecodeStatus decodeTrailers(ConnectionIt cnxIt, std::span<char> input, std::size_t& pos);

  bool parseHeadersUnchecked(SvToSvMap& headersMap, char* bufferBeg, char* first, char* last);

  bool maybeDecompressRequestBody(ConnectionIt cnxIt, bool usePerConnectionBodyStorage);

  // Handle Expect header tokens other than the built-in 100-continue, whose presence (in an HTTP/1.1 request) sets
  // found100Continue.
  // Returns true if processing should stop for this request (response already queued/sent): only its head is consumed.
  bool handleExpectHeader(ConnectionIt cnxIt, std::string_view expectHeader, const CorsPolicy* pCorsPolicy,
                          bool& found100Continue);

  // Build & queue an automatic HTTP -> HTTPS redirect response (used when config.httpsRedirect.enabled()).
  // Derives the target from the request Host header + config.httpsRedirect, then closes the connection.
  void emitHttpsRedirect(ConnectionIt cnxIt);

  // Answers the OPTIONS, TRACE and CONNECT requests that are not routed like the other ones. The current request ends
  // at requestEnd in the input buffer. Without bodyReady, an OPTIONS or TRACE response closes the connection.
  LoopAction processSpecialMethods(ConnectionIt& cnxIt, std::size_t requestEnd, const CorsPolicy* pCorsPolicy,
                                   bool bodyReady);

#ifdef AERONET_ENABLE_WEBSOCKET
  // Answer the WebSocket upgrade request of the connection, once its request middleware ran: checks its origin, then
  // switches the connection to WebSocket (SwitchProtocol), or answers it with an error (Continue, or Break when the
  // connection closes).
  LoopAction processWebSocketUpgrade(ConnectionIt cnxIt, const Router::RoutingResult& routingResult);
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Gives up the request of an async handler, its connection closing.
  static void AbandonAsyncHandler(AsyncHandlerState& async) noexcept;

  // Returns true if the handler was dispatched (it may have completed already), false if it returned an invalid task.
  // With bodyReady, consumedBytes is the size of the input buffer up to the end of the request (the requests served
  // before it included): erased once the handler completed, the input buffer not moving meanwhile. Without, and with
  // found100Continue, 100 Continue is sent once the handler waits for the body.
  bool dispatchAsyncHandler(ConnectionIt cnxIt, const SharedAsyncRequestHandler& handler, bool bodyReady,
                            bool isChunked, bool found100Continue, std::size_t consumedBytes,
                            const CorsPolicy* pCorsPolicy, std::span<const ResponseMiddleware> responseMiddleware,
                            std::size_t perRouteMaxBodyBytes);
  void handleAsyncBodyProgress(ConnectionIt cnxIt);
  void onAsyncHandlerCompleted(ConnectionIt cnxIt);
  // Sends the response of the completed async handler. With 'close', before its body is received: the connection then
  // closes.
  void tryFlushPendingAsyncResponse(ConnectionIt cnxIt, bool close = false);
  bool pinAsyncSharedBodyToConnectionStorage(ConnectionState& state) const;
#endif

  ServerContext* _pCtx;
};

}  // namespace internal
}  // namespace aeronet
