#include "aeronet/test_tls_http2_client.hpp"

#ifdef AERONET_POSIX
#include <poll.h>
#elifdef AERONET_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/toupperlower.hpp"

namespace aeronet::test {

std::string_view TlsHttp2Client::Response::header(std::string_view name) const noexcept {
  for (const auto& [hdrName, hdrValue] : headers) {
    if (CaseInsensitiveEqual(hdrName, name)) {
      return hdrValue;
    }
  }
  return {};
}

TlsHttp2Client::TlsHttp2Client(uint16_t port, Http2Config config)
    : _port(port),
      _tlsClient(
          port,
          TlsClient::Options{
              .alpn = {"h2"}, .clientCertPem = {}, .clientKeyPem = {}, .trustedServerCertPem = {}, .serverName = {}}),
      _http2Connection(std::make_unique<http2::Http2Connection>(config, false))  // client side
{
  if (!_tlsClient.handshakeOk()) {
    log::error("TLS handshake failed for HTTP/2 client");
    return;
  }

  if (_tlsClient.negotiatedAlpn() != "h2") {
    log::error("ALPN negotiation failed: expected 'h2', got '{}'", _tlsClient.negotiatedAlpn());
    return;
  }

  // Set up callbacks for response handling
  _http2Connection->setOnHeadersDecoded([this](uint32_t streamId, const HeadersViewMap& headers, bool endStream) {
    auto& streamResp = _streamResponses[streamId];
    streamResp.headersReceived = true;

    for (const auto& [name, value] : headers) {
      if (name == ":status") {
        streamResp.response.statusCode = 0;
        for (char ch : value) {
          streamResp.response.statusCode = (streamResp.response.statusCode * 10) + (ch - '0');
        }
      } else {
        streamResp.response.headers.emplace_back(name, value);
      }
    }

    if (endStream) {
      streamResp.complete = true;
    }
  });

  _http2Connection->setOnData([this](uint32_t streamId, std::span<const std::byte> data, bool endStream) {
    auto& streamResp = _streamResponses[streamId];
    streamResp.response.body.append(reinterpret_cast<const char*>(data.data()), data.size());
    if (endStream) {
      streamResp.complete = true;
    }
  });

  // Send client connection preface (magic string + SETTINGS)
  _http2Connection->sendClientPreface();
  if (_http2Connection->hasPendingOutput()) {
    auto output = _http2Connection->getPendingOutput();
    if (!writeAll(output)) {
      return;
    }
    _http2Connection->onOutputWritten(output.size());
  }

  // Process server's SETTINGS frame
  if (!processFrames(std::chrono::milliseconds{2000})) {
    return;
  }

  // Send SETTINGS ACK if needed
  if (_http2Connection->hasPendingOutput()) {
    auto output = _http2Connection->getPendingOutput();
    if (!writeAll(output)) {
      return;
    }
    _http2Connection->onOutputWritten(output.size());
  }

  _connected = _http2Connection->isOpen();
}

TlsHttp2Client::~TlsHttp2Client() {
  // Tests do not require graceful HTTP/2 session teardown. Avoiding any final
  // GOAWAY write here prevents large-transfer teardown from re-entering the
  // TLS write path after the test body has already completed.
}

bool TlsHttp2Client::isConnected() const noexcept { return _connected; }

std::string_view TlsHttp2Client::negotiatedAlpn() const noexcept { return _tlsClient.negotiatedAlpn(); }

TlsHttp2Client::Response TlsHttp2Client::get(std::string_view path,
                                             const std::vector<std::pair<std::string, std::string>>& extraHeaders) {
  return request("GET", path, extraHeaders, {});
}

TlsHttp2Client::Response TlsHttp2Client::post(std::string_view path, std::string_view body,
                                              std::string_view contentType,
                                              const std::vector<std::pair<std::string, std::string>>& extraHeaders) {
  std::vector<std::pair<std::string, std::string>> headers = extraHeaders;
  headers.emplace_back(http::ContentType, std::string(contentType));
  return request("POST", path, headers, body);
}

uint32_t TlsHttp2Client::sendAsyncRequest(std::string_view method, std::string_view path,
                                          const std::vector<std::pair<std::string, std::string>>& headers,
                                          std::string_view body) {
  if (!_connected) {
    log::warn("HTTP/2 client not connected");
    return 0;
  }
  return sendRequest(method, path, headers, body);
}

std::optional<TlsHttp2Client::Response> TlsHttp2Client::waitAndGetResponse(uint32_t streamId,
                                                                           std::chrono::milliseconds timeout) {
  if (!waitForResponse(streamId, timeout)) {
    log::error("Timeout waiting for response on stream {}", streamId);
    return std::nullopt;
  }

  auto iter = _streamResponses.find(streamId);
  if (iter == _streamResponses.end()) {
    return std::nullopt;
  }

  // Copy or move the response. Since it might be multiple streams, we copy for safety or just return a copy.
  return iter->second.response;
}

TlsHttp2Client::Response TlsHttp2Client::request(std::string_view method, std::string_view path,
                                                 const std::vector<std::pair<std::string, std::string>>& headers,
                                                 std::string_view body) {
  if (!_connected) {
    log::warn("HTTP/2 client not connected");
    return Response{};
  }

  uint32_t streamId = sendRequest(method, path, headers, body);
  if (streamId == 0) {
    log::error("HTTP/2 failed to send request");
    return Response{};
  }

  if (!waitForResponse(streamId, std::chrono::milliseconds{5000})) {
    log::error("Timeout waiting for response on stream {}", streamId);
    return Response{};
  }

  auto iter = _streamResponses.find(streamId);
  if (iter == _streamResponses.end()) {
    return Response{};
  }

  return std::move(iter->second.response);
}

bool TlsHttp2Client::writeAll(std::span<const std::byte> data) {
  bool ok = _tlsClient.writeAll(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
  // Reclaim any bytes that TlsClient::writeAll SSL_read'd while waiting for
  // POLLOUT (TCP deadlock prevention) and queue them for HTTP/2 processing.
  auto& drained = _tlsClient.drainedDuringWrite();
  if (!drained.empty()) {
    _pendingInput.insert(_pendingInput.end(), reinterpret_cast<const std::byte*>(drained.data()),
                         reinterpret_cast<const std::byte*>(drained.data() + drained.size()));
    drained.clear();
  }
  return ok;
}

bool TlsHttp2Client::processFrames(std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;

  // Allow at least one iteration even with a zero timeout so callers can
  // drain already-buffered TLS data without paying a 1ms poll penalty.
  bool firstIteration = true;

  while (firstIteration || std::chrono::steady_clock::now() < deadline) {
    firstIteration = false;

    // Try to read data from TLS connection
    std::array<char, 16384> buffer{};
    int fd = _tlsClient.fd();

    // Skip the poll when SSL or our own buffer already has data to process;
    // but always attempt a non-blocking readSome so an incomplete frame in
    // _pendingInput can be completed with fresh socket bytes.
    bool sslHasPending = (::SSL_pending(_tlsClient.sslHandle()) > 0);
    bool hasPendingInput = (_pendingInput.size() > _pendingOffset);

    if (!sslHasPending && !hasPendingInput) {
      auto remainingMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      int pollMs = (remainingMs <= 0)
                       ? 0
                       : std::max(1, static_cast<int>(std::min(remainingMs, static_cast<decltype(remainingMs)>(100))));
      int ret = 0;
#ifdef AERONET_WINDOWS
      // select() is used instead of WSAPoll() — WSAPoll has a bug on Windows
      // where it can fail to report readability on non-blocking loopback sockets.
      fd_set rfds{};
      FD_ZERO(&rfds);
      FD_SET(static_cast<SOCKET>(fd), &rfds);
      struct timeval tv{};
      tv.tv_sec = pollMs / 1000;
      tv.tv_usec = (pollMs % 1000) * 1000;
      ret = ::select(0, &rfds, nullptr, nullptr, &tv);
#else
      struct pollfd pfd{};  // NOLINT(misc-include-cleaner)
      pfd.fd = fd;
      pfd.events = POLLIN;  // NOLINT(misc-include-cleaner)
      // NOLINTNEXTLINE(misc-include-cleaner)
      ret = ::poll(&pfd, 1, pollMs);
#endif
      if (ret < 0) {
        return false;
      }
      if (ret == 0) {
        // Timeout - check if we have what we need
        if (_http2Connection->state() == http2::ConnectionState::Open) {
          return true;
        }
        continue;
      }
    }

    // Non-blocking read — always attempt even when hasPendingInput is true,
    // so that an incomplete frame in _pendingInput can be completed with
    // fresh socket bytes.
    {
      auto data = _tlsClient.readSome(buffer);
      if (!data.empty()) {
        _pendingInput.insert(_pendingInput.end(), reinterpret_cast<const std::byte*>(data.data()),
                             reinterpret_cast<const std::byte*>(data.data() + data.size()));
      } else if (hasPendingInput) {
        // Incomplete frame and no bytes available without blocking — wait.
        auto remainingMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        int pollMs =
            (remainingMs <= 0)
                ? 0
                : std::max(1, static_cast<int>(std::min(remainingMs, static_cast<decltype(remainingMs)>(100))));
#ifdef AERONET_WINDOWS
        fd_set rfds2{};
        FD_ZERO(&rfds2);
        FD_SET(static_cast<SOCKET>(fd), &rfds2);
        struct timeval tv2{};
        tv2.tv_sec = pollMs / 1000;
        tv2.tv_usec = (pollMs % 1000) * 1000;
        int ret2 = ::select(0, &rfds2, nullptr, nullptr, &tv2);
#else
        struct pollfd pfd2{};
        pfd2.fd = fd;
        pfd2.events = POLLIN;
        int ret2 = ::poll(&pfd2, 1, pollMs);
#endif
        if (ret2 <= 0) {
          continue;
        }
        data = _tlsClient.readSome(buffer);
        if (!data.empty()) {
          _pendingInput.insert(_pendingInput.end(), reinterpret_cast<const std::byte*>(data.data()),
                               reinterpret_cast<const std::byte*>(data.data() + data.size()));
        }
      }
    }
    if (_pendingInput.size() <= _pendingOffset) {
      continue;  // Nothing to process
    }

    processPendingInput();

    // Check if we've completed handshake
    if (_http2Connection->state() == http2::ConnectionState::Open) {
      return true;
    }
  }

  return _http2Connection->state() == http2::ConnectionState::Open;
}

bool TlsHttp2Client::waitForResponse(uint32_t streamId, std::chrono::milliseconds timeout, bool waitForComplete) {
  auto deadline = std::chrono::steady_clock::now() + timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    auto iter = _streamResponses.find(streamId);
    if (iter != _streamResponses.end()) {
      if (waitForComplete && iter->second.complete) {
        return true;
      }
      if (!waitForComplete && iter->second.headersReceived) {
        return true;
      }
    }

    // Read and process more frames
    std::array<char, 16384> buffer{};
    int fd = _tlsClient.fd();

    // Process any data already queued (e.g. drained during a prior writeAll
    // WANT_WRITE) before blocking on the socket.
    bool sslHasPending = (::SSL_pending(_tlsClient.sslHandle()) > 0);
    bool hasPendingInput = (_pendingInput.size() > _pendingOffset);

    if (!sslHasPending && !hasPendingInput) {
      auto remainingMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      int pollMs = std::max(1, static_cast<int>(std::min(remainingMs, static_cast<decltype(remainingMs)>(100))));
      int ret = 0;
#ifdef AERONET_WINDOWS
      // select() instead of WSAPoll() — WSAPoll has a bug on Windows where it
      // can fail to report readability on non-blocking loopback sockets.
      fd_set rfds{};
      FD_ZERO(&rfds);
      FD_SET(static_cast<SOCKET>(fd), &rfds);
      struct timeval tv{};
      tv.tv_sec = pollMs / 1000;
      tv.tv_usec = (pollMs % 1000) * 1000;
      ret = ::select(0, &rfds, nullptr, nullptr, &tv);
#else
      struct pollfd pfd{};
      pfd.fd = fd;
      pfd.events = POLLIN;
      ret = ::poll(&pfd, 1, pollMs);
#endif
      if (ret <= 0) {
        continue;
      }
    }

    // Always attempt a non-blocking read, even when hasPendingInput is true.
    // An incomplete HTTP/2 frame may be sitting in _pendingInput waiting for
    // more bytes; skipping readSome would spin forever consuming 0 bytes.
    {
      auto data = _tlsClient.readSome(buffer);
      if (!data.empty()) {
        _pendingInput.insert(_pendingInput.end(), reinterpret_cast<const std::byte*>(data.data()),
                             reinterpret_cast<const std::byte*>(data.data() + data.size()));
      } else if (hasPendingInput) {
        // Incomplete frame in buffer but no bytes available without blocking.
        // Wait for the socket to become readable before retrying.
        auto remainingMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remainingMs <= 0) {
          break;
        }
        int pollMs = std::max(1, static_cast<int>(std::min(remainingMs, static_cast<decltype(remainingMs)>(100))));
#ifdef AERONET_WINDOWS
        fd_set rfds{};
        FD_ZERO(&rfds);
        FD_SET(static_cast<SOCKET>(fd), &rfds);
        struct timeval tv{};
        tv.tv_sec = pollMs / 1000;
        tv.tv_usec = (pollMs % 1000) * 1000;
        int ret = ::select(0, &rfds, nullptr, nullptr, &tv);
#else
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, pollMs);
#endif
        if (ret <= 0) {
          continue;
        }
        data = _tlsClient.readSome(buffer);
        if (!data.empty()) {
          _pendingInput.insert(_pendingInput.end(), reinterpret_cast<const std::byte*>(data.data()),
                               reinterpret_cast<const std::byte*>(data.data() + data.size()));
        }
      }
    }

    processPendingInput();
  }

  auto iter = _streamResponses.find(streamId);
  if (iter == _streamResponses.end()) {
    return false;
  }
  return waitForComplete ? iter->second.complete : iter->second.headersReceived;
}

uint32_t TlsHttp2Client::sendRequest(std::string_view method, std::string_view path,
                                     const std::vector<std::pair<std::string, std::string>>& headers,
                                     std::string_view body) {
  uint32_t streamId = _nextStreamId;
  _nextStreamId += 2;  // Client streams are odd-numbered

  bool endStream = body.empty();

  // Send HEADERS frame
  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", method));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", std::string("localhost:") + std::to_string(_port)));
  hdrs.append(MakeHttp1HeaderLine(":path", path));

  for (const auto& [name, value] : headers) {
    std::string lname = name;
    std::ranges::transform(lname, lname.begin(), [](char ch) { return tolower(ch); });
    hdrs.append(MakeHttp1HeaderLine(lname, value));
  }
  auto err = _http2Connection->sendHeaders(streamId, http::StatusCode{}, HeadersView(hdrs), endStream);

  if (err != http2::ErrorCode::NoError) {
    log::error("Failed to send HEADERS: {}", http2::ErrorCodeName(err));
    return 0;
  }

  // Send pending output
  if (_http2Connection->hasPendingOutput()) {
    auto output = _http2Connection->getPendingOutput();
    if (!writeAll(output)) {
      return 0;
    }
    _http2Connection->onOutputWritten(output.size());
  }

  // Send DATA frame if there's a body
  if (!body.empty()) {
    std::span<const std::byte> bodyData(reinterpret_cast<const std::byte*>(body.data()), body.size());
    err = _http2Connection->sendData(streamId, bodyData, true);
    if (err != http2::ErrorCode::NoError) {
      log::error("Failed to send DATA: {}", http2::ErrorCodeName(err));
      return 0;
    }

    if (_http2Connection->hasPendingOutput()) {
      auto output = _http2Connection->getPendingOutput();
      if (!writeAll(output)) {
        return 0;
      }
      _http2Connection->onOutputWritten(output.size());
    }
  }

  // Initialize response tracking
  _streamResponses[streamId] = StreamResponse{};

  return streamId;
}

uint32_t TlsHttp2Client::connect(std::string_view authority,
                                 const std::vector<std::pair<std::string, std::string>>& headers) {
  if (!_connected) {
    log::warn("HTTP/2 client not connected");
    return 0;
  }

  uint32_t streamId = _nextStreamId;
  _nextStreamId += 2;

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  hdrs.append(MakeHttp1HeaderLine(":authority", authority));

  for (const auto& [name, value] : headers) {
    std::string lname = name;
    std::ranges::transform(lname, lname.begin(), [](char ch) { return tolower(ch); });
    hdrs.append(MakeHttp1HeaderLine(lname, value));
  }

  auto err = _http2Connection->sendHeaders(streamId, http::StatusCode{}, HeadersView(hdrs), false);
  if (err != http2::ErrorCode::NoError) {
    log::error("Failed to send CONNECT HEADERS: {}", http2::ErrorCodeName(err));
    return 0;
  }

  if (_http2Connection->hasPendingOutput()) {
    auto output = _http2Connection->getPendingOutput();
    if (!writeAll(output)) {
      return 0;
    }
    _http2Connection->onOutputWritten(output.size());
  }

  _streamResponses[streamId] = StreamResponse{};

  // Wait for 200 OK response
  if (!waitForResponse(streamId, std::chrono::milliseconds{5000}, false)) {
    log::error("Timeout waiting for CONNECT response on stream {}", streamId);
    return 0;
  }

  auto iter = _streamResponses.find(streamId);
  if (iter == _streamResponses.end() || iter->second.response.statusCode != 200) {
    log::error("CONNECT failed with status {}", iter != _streamResponses.end() ? iter->second.response.statusCode : 0);
    return 0;
  }

  return streamId;
}

void TlsHttp2Client::processPendingInput() {
  for (;;) {
    std::span<const std::byte> inputData(_pendingInput.data() + _pendingOffset, _pendingInput.size() - _pendingOffset);
    if (inputData.empty()) {
      break;
    }

    auto result = _http2Connection->processInput(inputData);

    if (result.bytesConsumed > 0) {
      _pendingOffset += result.bytesConsumed;
      if (_pendingOffset == _pendingInput.size()) {
        _pendingInput.clear();
        _pendingOffset = 0;
      } else if (_pendingOffset > (64UL * 1024UL)) {
        _pendingInput.erase(_pendingInput.begin(), _pendingInput.begin() + static_cast<std::ptrdiff_t>(_pendingOffset));
        _pendingOffset = 0;
      }
    }

    if (_http2Connection->hasPendingOutput()) {
      auto output = _http2Connection->getPendingOutput();
      if (!writeAll(output)) {
        return;
      }
      _http2Connection->onOutputWritten(output.size());
    }

    if (result.action == http2::Http2Connection::ProcessResult::Action::Error ||
        result.action == http2::Http2Connection::ProcessResult::Action::Closed ||
        result.action == http2::Http2Connection::ProcessResult::Action::GoAway) {
      return;
    }

    if (result.bytesConsumed == 0) {
      break;
    }
  }
}

bool TlsHttp2Client::sendTunnelData(uint32_t streamId, std::span<const std::byte> data, bool endStream) {
  if (!_connected) {
    return false;
  }

  auto err = _http2Connection->sendData(streamId, data, endStream);
  if (err != http2::ErrorCode::NoError) {
    log::error("Failed to send tunnel DATA: {}", http2::ErrorCodeName(err));
    return false;
  }

  if (_http2Connection->hasPendingOutput()) {
    auto output = _http2Connection->getPendingOutput();
    if (!writeAll(output)) {
      return false;
    }
    _http2Connection->onOutputWritten(output.size());
  }

  // Process any data that TlsClient::writeAll drained from the socket while
  // waiting for POLLOUT (TCP deadlock prevention).  Doing this here ensures
  // that WINDOW_UPDATE frames sent by the remote end are consumed immediately,
  // which keeps H2 flow-control windows open and prevents a secondary deadlock.
  if (_pendingInput.size() > _pendingOffset) {
    processPendingInput();
  }

  return true;
}

void TlsHttp2Client::receiveTunnelData(RawChars& out, uint32_t streamId, std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;

  std::array<char, 32U << 10> buffer{};
  bool firstIteration = true;

  while (true) {
    // Consume any body already parsed by the H2 layer.
    {
      auto iter = _streamResponses.find(streamId);
      if (iter != _streamResponses.end() && !iter->second.response.body.empty()) {
        auto& body = iter->second.response.body;
        out.append(body.data(), body.size());
        body.clear();
        return;
      }
      if (iter != _streamResponses.end() && iter->second.complete) {
        return;
      }
    }

    int fd = _tlsClient.fd();
    bool sslHasPending = (::SSL_pending(_tlsClient.sslHandle()) > 0);
    bool hasPendingInput = (_pendingInput.size() > _pendingOffset);

    // If there's no data immediately available, check whether we have time left.
    if (!sslHasPending && !hasPendingInput) {
      auto nowMs = std::chrono::steady_clock::now();
      bool timeExpired = !firstIteration && (nowMs >= deadline);

      auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - nowMs).count();
      // Always do at least one poll (even with 0ms) so we flush any socket bytes.
      int pollMs = timeExpired
                       ? 0
                       : std::max(1, static_cast<int>(std::min(remainingMs, static_cast<decltype(remainingMs)>(100))));
      int ret = 0;
#ifdef AERONET_WINDOWS
      // select() instead of WSAPoll() — WSAPoll has a bug on Windows where it
      // can fail to report readability on non-blocking loopback sockets.
      fd_set rfds{};
      FD_ZERO(&rfds);
      FD_SET(static_cast<SOCKET>(fd), &rfds);
      struct timeval tv{};
      tv.tv_sec = pollMs / 1000;
      tv.tv_usec = (pollMs % 1000) * 1000;
      ret = ::select(0, &rfds, nullptr, nullptr, &tv);
#else
      struct pollfd pfd{};  // NOLINT(misc-include-cleaner)
      pfd.fd = fd;
      pfd.events = POLLIN;  // NOLINT(misc-include-cleaner)
      // NOLINTNEXTLINE(misc-include-cleaner)
      ret = ::poll(&pfd, 1, pollMs);
#endif
      firstIteration = false;
      if (ret <= 0) {
        // Nothing available right now; stop if deadline reached, else retry.
        if (timeExpired) {
          return;
        }
        continue;
      }
    } else {
      firstIteration = false;
    }

    // Read whatever is available right now (non-blocking).
    {
      auto data = _tlsClient.readSome(buffer);
      if (!data.empty()) {
        _pendingInput.insert(_pendingInput.end(), reinterpret_cast<const std::byte*>(data.data()),
                             reinterpret_cast<const std::byte*>(data.data() + data.size()));
      } else if (!sslHasPending && _pendingInput.size() > _pendingOffset) {
        // hasPendingInput was true but readSome produced nothing — we have a
        // partial H2 frame and need more socket bytes to complete it.  Poll
        // briefly so we don't spin at 100% CPU while waiting for the server to
        // send the rest of the frame.  Honour the deadline to avoid hanging.
        auto remainingMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remainingMs <= 0) {
          return;
        }
        int pollMs = std::max(1, static_cast<int>(std::min(remainingMs, static_cast<decltype(remainingMs)>(100))));
#ifdef AERONET_WINDOWS
        // select() instead of WSAPoll() — see above.
        fd_set rfds{};
        FD_ZERO(&rfds);
        FD_SET(static_cast<SOCKET>(fd), &rfds);
        struct timeval tv{};
        tv.tv_sec = pollMs / 1000;
        tv.tv_usec = (pollMs % 1000) * 1000;
        ::select(0, &rfds, nullptr, nullptr, &tv);
#else
        struct pollfd pfd{};  // NOLINT(misc-include-cleaner)
        pfd.fd = fd;
        pfd.events = POLLIN;  // NOLINT(misc-include-cleaner)
        // NOLINTNEXTLINE(misc-include-cleaner)
        ::poll(&pfd, 1, pollMs);
#endif
      }
    }

    processPendingInput();
  }
}

}  // namespace aeronet::test
