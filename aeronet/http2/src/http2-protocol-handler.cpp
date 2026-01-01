#include "aeronet/http2-protocol-handler.hpp"

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/log.hpp"
#include "aeronet/path-handler-entry.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/request-task.hpp"
#include "aeronet/router.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::http2 {

// ============================
// Http2ProtocolHandler
// ============================

Http2ProtocolHandler::Http2ProtocolHandler(const Http2Config& config, Router& router)
    : _connection(config, true), _pRouter(&router) {
  setupCallbacks();
}

Http2ProtocolHandler::~Http2ProtocolHandler() = default;

Http2ProtocolHandler::Http2ProtocolHandler(Http2ProtocolHandler&&) noexcept = default;
Http2ProtocolHandler& Http2ProtocolHandler::operator=(Http2ProtocolHandler&&) noexcept = default;

void Http2ProtocolHandler::setupCallbacks() {
  _connection.setOnHeaders([this](uint32_t streamId, const HeaderProvider& headerProvider, bool endStream) {
    onHeadersReceived(streamId, headerProvider, endStream);
  });

  _connection.setOnData([this](uint32_t streamId, std::span<const std::byte> data, bool endStream) {
    onDataReceived(streamId, data, endStream);
  });

  _connection.setOnStreamClosed([this](uint32_t streamId) { onStreamClosed(streamId); });

  _connection.setOnStreamReset([this](uint32_t streamId, ErrorCode errorCode) { onStreamReset(streamId, errorCode); });
}

ProtocolProcessResult Http2ProtocolHandler::processInput(std::span<const std::byte> data,
                                                         [[maybe_unused]] ::aeronet::ConnectionState& state) {
  auto result = _connection.processInput(data);

  ProtocolProcessResult output;
  output.bytesConsumed = result.bytesConsumed;

  switch (result.action) {
    case Http2Connection::ProcessResult::Action::Continue:
      output.action = ProtocolProcessResult::Action::Continue;
      break;
    case Http2Connection::ProcessResult::Action::OutputReady:
      output.action = ProtocolProcessResult::Action::ResponseReady;
      break;
    case Http2Connection::ProcessResult::Action::Error:
      output.action = ProtocolProcessResult::Action::CloseImmediate;
      log::error("HTTP/2 protocol error: {} ({})", result.errorMessage, ErrorCodeName(result.errorCode));
      break;
    case Http2Connection::ProcessResult::Action::GoAway:
      [[fallthrough]];
    case Http2Connection::ProcessResult::Action::Closed:
      output.action = ProtocolProcessResult::Action::Close;
      break;
  }

  return output;
}

bool Http2ProtocolHandler::hasPendingOutput() const noexcept { return _connection.hasPendingOutput(); }

std::span<const std::byte> Http2ProtocolHandler::getPendingOutput() { return _connection.getPendingOutput(); }

void Http2ProtocolHandler::onOutputWritten(std::size_t bytesWritten) { _connection.onOutputWritten(bytesWritten); }

void Http2ProtocolHandler::initiateClose() { _connection.initiateGoAway(ErrorCode::NoError); }

void Http2ProtocolHandler::onTransportClosing() {
  // Clean up any pending state
  _streamRequests.clear();
}

namespace {

http::Method ParseHttpMethod(std::string_view method) noexcept {
  if (method == "GET") {
    return http::Method::GET;
  }
  if (method == "POST") {
    return http::Method::POST;
  }
  if (method == "PUT") {
    return http::Method::PUT;
  }
  if (method == "DELETE") {
    return http::Method::DELETE;
  }
  if (method == "HEAD") {
    return http::Method::HEAD;
  }
  if (method == "OPTIONS") {
    return http::Method::OPTIONS;
  }
  if (method == "PATCH") {
    return http::Method::PATCH;
  }
  if (method == "CONNECT") {
    return http::Method::CONNECT;
  }
  if (method == "TRACE") {
    return http::Method::TRACE;
  }
  // Fallback to GET for unknown methods
  return http::Method::GET;
}

}  // namespace

void Http2ProtocolHandler::onHeadersReceived(uint32_t streamId, const HeaderProvider& headerProvider, bool endStream) {
  // Get or create stream request
  StreamRequest& streamReq = _streamRequests[streamId];

  // Create a fresh HttpRequest - use placement new since default ctor is private but we're a friend
  streamReq.request = HttpRequest();
  HttpRequest& req = streamReq.request;

  streamReq.headersReceived = true;

  // Clear previous header storage and rebuild
  streamReq.headerStorage.clear();

  // Temporary storage for pseudo-headers before we can set them
  std::string_view methodStr;
  std::string_view schemeStr;
  std::string_view authorityStr;
  std::string_view pathStr;

  // Two-pass approach to avoid use-after-free:
  // Pass 1: Collect all headers into storage and record offsets
  // Pass 2: Create string_views after all data is stored (no more reallocations)

  struct HeaderOffsets {
    std::size_t nameStart;
    std::size_t nameLen;
    std::size_t valueStart;
    std::size_t valueLen;
    bool isPseudo;
  };

  // TODO: looping allocations can be avoided?
  vector<HeaderOffsets> headerOffsets;

  // Pass 1: Copy all header data and record offsets
  headerProvider([&streamReq, &headerOffsets](std::string_view name, std::string_view value) {
    HeaderOffsets offsets;
    offsets.nameStart = streamReq.headerStorage.size();
    offsets.nameLen = name.size();
    streamReq.headerStorage.append(name);
    offsets.valueStart = streamReq.headerStorage.size();
    offsets.valueLen = value.size();
    streamReq.headerStorage.append(value);
    offsets.isPseudo = !name.empty() && name[0] == ':';
    headerOffsets.push_back(std::move(offsets));
  });

  // Pass 2: Now that storage is stable, create string_views and populate request
  const char* storageBase = streamReq.headerStorage.data();
  for (const auto& offsets : headerOffsets) {
    std::string_view storedName(storageBase + offsets.nameStart, offsets.nameLen);
    std::string_view storedValue(storageBase + offsets.valueStart, offsets.valueLen);

    if (offsets.isPseudo) {
      if (storedName == ":method") {
        methodStr = storedValue;
      } else if (storedName == ":scheme") {
        schemeStr = storedValue;
      } else if (storedName == ":authority") {
        authorityStr = storedValue;
      } else if (storedName == ":path") {
        pathStr = storedValue;
      }
    } else {
      req._headers[storedName] = storedValue;
    }
  }

  // Set HTTP/2 specific fields on HttpRequest
  req._streamId = streamId;
  req._scheme = schemeStr;
  req._authority = authorityStr;
  req._path = pathStr;
  req._method = ParseHttpMethod(methodStr);
  req._version = http::HTTP_2_0;
  req._reqStart = std::chrono::steady_clock::now();

  // If END_STREAM is set, dispatch immediately (no body expected)
  if (endStream) {
    dispatchRequest(streamId);
  }
}

void Http2ProtocolHandler::onDataReceived(uint32_t streamId, std::span<const std::byte> data, bool endStream) {
  auto iter = _streamRequests.find(streamId);
  if (iter == _streamRequests.end()) [[unlikely]] {
    log::warn("HTTP/2 DATA frame for unknown stream {}", streamId);
    return;
  }

  StreamRequest& streamReq = iter->second;

  // Accumulate body data
  streamReq.bodyBuffer.append(reinterpret_cast<const char*>(data.data()), data.size());

  if (endStream) {
    // Set body on HttpRequest
    streamReq.request._body = std::string_view(streamReq.bodyBuffer.data(), streamReq.bodyBuffer.size());
    dispatchRequest(streamId);
  }
}

void Http2ProtocolHandler::onStreamClosed(uint32_t streamId) { _streamRequests.erase(streamId); }

void Http2ProtocolHandler::onStreamReset(uint32_t streamId, ErrorCode errorCode) {
  log::debug("HTTP/2 stream {} reset with error: {}", streamId, ErrorCodeName(errorCode));
  _streamRequests.erase(streamId);
}

void Http2ProtocolHandler::dispatchRequest(uint32_t streamId) {
  auto iter = _streamRequests.find(streamId);
  if (iter == _streamRequests.end()) {
    log::error("HTTP/2 dispatchRequest for unknown stream {}", streamId);
    return;
  }

  HttpRequest& request = iter->second.request;

  // Validate required pseudo-headers
  if (request.path().empty() && request.method() != http::Method::CONNECT) {
    log::error("HTTP/2 stream {} missing :path", streamId);
    _connection.sendRstStream(streamId, ErrorCode::ProtocolError);
    _streamRequests.erase(iter);
    return;
  }

  ErrorCode err;

  // Dispatch to the callback provided by SingleHttpServer
  try {
    err = sendResponse(streamId, reply(request));
    if (err != ErrorCode::NoError) [[unlikely]] {
      log::error("HTTP/2 failed to send response on stream {}: {}", streamId, ErrorCodeName(err));
    }
  } catch (const std::exception& ex) {
    log::error("HTTP/2 dispatcher exception on stream {}: {}", streamId, ex.what());
    err = sendResponse(streamId, HttpResponse(http::StatusCodeInternalServerError).body(ex.what()));
  } catch (...) {
    log::error("HTTP/2 unknown exception on stream {}", streamId);
    err = sendResponse(streamId, HttpResponse(http::StatusCodeInternalServerError).body("Unknown error"));
  }

  // Clean up stream request
  _streamRequests.erase(iter);
}

HttpResponse Http2ProtocolHandler::reply(HttpRequest& request) {
  auto routingResult = _pRouter->match(request.method(), request.path());
  // Set path parameters if any
  for (const auto& param : routingResult.pathParams) {
    request._pathParams[param.key] = param.value;
  }

  // Check path-specific HTTP/2 configuration
  const auto pathHttp2Mode = routingResult.pathConfig.http2Enable;
  if (pathHttp2Mode == PathEntryConfig::Http2Enable::Disable) {
    // HTTP/2 is explicitly disabled for this path
    return HttpResponse(http::StatusCodeNotFound);
  }

  // Handle the request based on handler type
  if (const auto* reqHandler = routingResult.requestHandler(); reqHandler != nullptr) {
    return (*reqHandler)(request);
  }

  if (const auto* asyncHandler = routingResult.asyncRequestHandler(); asyncHandler != nullptr) {
    // Async handlers: run the coroutine to completion synchronously
    // HTTP/2 streams are independent and don't block each other
    auto task = (*asyncHandler)(request);
    if (task.valid()) {
      auto handle = task.release();
      while (!handle.done()) {
        handle.resume();
      }
      auto typedHandle = std::coroutine_handle<RequestTask<HttpResponse>::promise_type>::from_address(handle.address());
      HttpResponse resp = std::move(typedHandle.promise().consume_result());
      typedHandle.destroy();
      return resp;
    }
    log::error("HTTP/2 async handler returned invalid task for path {}", request._path);
    return HttpResponse(http::StatusCodeInternalServerError).body("Async handler inactive");
  }

  if (routingResult.streamingHandler() != nullptr) {
    // Streaming handlers not yet supported for HTTP/2
    // Full implementation requires:
    // 1. Create Http2ResponseWriter that emits HEADERS frame when headers are first sent
    // 2. Emit DATA frames for each writeBody() call (respecting peer's maxFrameSize)
    // 3. Handle HTTP/2 flow control (may need to pause/resume when window exhausted)
    // 4. Emit trailers as final HEADERS frame with END_STREAM
    // 5. Integrate with the event loop for backpressure handling
    log::warn("Streaming handlers not yet fully supported for HTTP/2, path: {}", request._path);
    return HttpResponse(http::StatusCodeNotImplemented).body("Streaming handlers not yet supported for HTTP/2");
  }

  if (routingResult.methodNotAllowed) {
    return HttpResponse(http::StatusCodeMethodNotAllowed);
  }

  return HttpResponse(http::StatusCodeNotFound);
}

ErrorCode Http2ProtocolHandler::sendResponse(uint32_t streamId, HttpResponse response) {
  const auto body = response.body();
  const bool hasBody = !body.empty();
  const auto trailers = response.trailers();
  const bool hasTrailers = trailers.begin() != trailers.end();

  // Determine when to set END_STREAM:
  // - On HEADERS if no body and no trailers
  // - On DATA if body present but no trailers
  // - On trailer HEADERS if trailers present
  const bool endStreamOnHeaders = !hasBody && !hasTrailers;
  const bool endStreamOnData = hasBody && !hasTrailers;

  // Send HEADERS frame (response headers)
  auto err = _connection.sendHeaders(
      streamId,
      [&response](const HeaderCallback& emit) {
        // Emit :status pseudo-header first
        emit(":status", response.statusStr());

        // Emit regular headers
        for (const auto [name, value] : response.headers()) {
          emit(name, value);
        }
      },
      endStreamOnHeaders);

  if (err != ErrorCode::NoError) {
    return err;
  }

  // Send DATA frame(s) with body if present
  // Note: sendData() already handles splitting into multiple frames based on peer's maxFrameSize
  if (hasBody) {
    err = _connection.sendData(streamId, std::as_bytes(std::span<const char>(body)), endStreamOnData);
    if (err != ErrorCode::NoError) {
      return err;
    }
  }

  // Send trailers as a HEADERS frame with END_STREAM (RFC 9113 §8.1)
  if (hasTrailers) {
    err = _connection.sendHeaders(
        streamId,
        [&trailers](const HeaderCallback& emit) {
          for (const auto [name, value] : trailers) {
            emit(name, value);
          }
        },
        true);  // END_STREAM must be set on trailers
  }

  return err;
}

std::unique_ptr<IProtocolHandler> CreateHttp2ProtocolHandler(const Http2Config& config, Router& router,
                                                             bool sendServerPrefaceForTls) {
  auto protocolHandler = std::make_unique<Http2ProtocolHandler>(config, router);
  if (sendServerPrefaceForTls) {
    // For TLS ALPN "h2", the server must send SETTINGS immediately after TLS handshake
    protocolHandler->connection().sendServerPreface();
  }
  return protocolHandler;
}

}  // namespace aeronet::http2
