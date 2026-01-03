#include "aeronet/test_tls_http2_client.hpp"

#include <poll.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/headers-view-map.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/log.hpp"
#include "aeronet/string-equal-ignore-case.hpp"

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
      log::error("Failed to send HTTP/2 connection preface");
      return;
    }
    _http2Connection->onOutputWritten(output.size());
  }

  // Process server's SETTINGS frame
  if (!processFrames(std::chrono::milliseconds{2000})) {
    log::error("Failed to process server SETTINGS");
    return;
  }

  // Send SETTINGS ACK if needed
  if (_http2Connection->hasPendingOutput()) {
    auto output = _http2Connection->getPendingOutput();
    if (!writeAll(output)) {
      log::error("Failed to send SETTINGS ACK");
      return;
    }
    _http2Connection->onOutputWritten(output.size());
  }

  _connected = _http2Connection->isOpen();
  if (_connected) {
    log::debug("HTTP/2 client connected successfully");
  }
}

TlsHttp2Client::~TlsHttp2Client() {
  if (_connected && _http2Connection) {
    _http2Connection->initiateGoAway(http2::ErrorCode::NoError);
    if (_http2Connection->hasPendingOutput()) {
      auto output = _http2Connection->getPendingOutput();
      writeAll(output);
    }
  }
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
  headers.emplace_back("content-type", std::string(contentType));
  return request("POST", path, headers, body);
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
  return _tlsClient.writeAll(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

bool TlsHttp2Client::processFrames(std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;

  std::vector<std::byte> pending;
  std::size_t pendingOffset = 0;

  while (std::chrono::steady_clock::now() < deadline) {
    // Try to read data from TLS connection
    std::array<char, 16384> buffer{};
    int fd = _tlsClient.fd();

    struct pollfd pfd{};  // NOLINT(misc-include-cleaner) poll.h is the correct include
    pfd.fd = fd;
    pfd.events = POLLIN;  // NOLINT(misc-include-cleaner)

    // NOLINTNEXTLINE(misc-include-cleaner)
    int ret = ::poll(&pfd, 1, 100);  // 100ms poll timeout
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

    // Read from TLS
    auto data = _tlsClient.readSome(buffer);
    if (data.empty()) {
      continue;
    }

    pending.insert(pending.end(), reinterpret_cast<const std::byte*>(data.data()),
                   reinterpret_cast<const std::byte*>(data.data() + data.size()));

    // Process through HTTP/2 connection
    for (;;) {
      std::span<const std::byte> inputData(pending.data() + pendingOffset, pending.size() - pendingOffset);
      if (inputData.empty()) {
        break;
      }

      auto result = _http2Connection->processInput(inputData);

      if (result.bytesConsumed > 0) {
        pendingOffset += result.bytesConsumed;
        if (pendingOffset == pending.size()) {
          pending.clear();
          pendingOffset = 0;
        } else if (pendingOffset > (64UL * 1024UL)) {
          pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(pendingOffset));
          pendingOffset = 0;
        }
      }

      // Send any pending output (SETTINGS ACK, WINDOW_UPDATE, etc.)
      if (_http2Connection->hasPendingOutput()) {
        auto output = _http2Connection->getPendingOutput();
        if (!writeAll(output)) {
          return false;
        }
        _http2Connection->onOutputWritten(output.size());
      }

      if (result.action == http2::Http2Connection::ProcessResult::Action::Error) {
        log::error("HTTP/2 protocol error: {}", result.errorMessage);
        return false;
      }

      if (result.action == http2::Http2Connection::ProcessResult::Action::Closed ||
          result.action == http2::Http2Connection::ProcessResult::Action::GoAway) {
        return false;
      }

      if (result.bytesConsumed == 0) {
        break;  // Need more data
      }
    }

    // Check if we've completed handshake
    if (_http2Connection->state() == http2::ConnectionState::Open) {
      return true;
    }
  }

  return _http2Connection->state() == http2::ConnectionState::Open;
}

bool TlsHttp2Client::waitForResponse(uint32_t streamId, std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;

  std::vector<std::byte> pending;
  std::size_t pendingOffset = 0;

  while (std::chrono::steady_clock::now() < deadline) {
    auto iter = _streamResponses.find(streamId);
    if (iter != _streamResponses.end() && iter->second.complete) {
      return true;
    }

    // Read and process more frames
    std::array<char, 16384> buffer{};
    int fd = _tlsClient.fd();

    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    int ret = ::poll(&pfd, 1, 100);
    if (ret <= 0) {
      continue;
    }

    auto data = _tlsClient.readSome(buffer);
    if (data.empty()) {
      continue;
    }

    pending.insert(pending.end(), reinterpret_cast<const std::byte*>(data.data()),
                   reinterpret_cast<const std::byte*>(data.data() + data.size()));

    for (;;) {
      std::span<const std::byte> inputData(pending.data() + pendingOffset, pending.size() - pendingOffset);
      if (inputData.empty()) {
        break;
      }

      auto result = _http2Connection->processInput(inputData);

      if (result.bytesConsumed > 0) {
        pendingOffset += result.bytesConsumed;
        if (pendingOffset == pending.size()) {
          pending.clear();
          pendingOffset = 0;
        } else if (pendingOffset > (64UL * 1024UL)) {
          pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(pendingOffset));
          pendingOffset = 0;
        }
      }

      if (_http2Connection->hasPendingOutput()) {
        auto output = _http2Connection->getPendingOutput();
        writeAll(output);
        _http2Connection->onOutputWritten(output.size());
      }

      if (result.action == http2::Http2Connection::ProcessResult::Action::Error) {
        log::error("HTTP/2 client protocol error while waiting for response: {} ({})", result.errorMessage,
                   http2::ErrorCodeName(result.errorCode));
        return false;
      }

      if (result.action == http2::Http2Connection::ProcessResult::Action::Closed ||
          result.action == http2::Http2Connection::ProcessResult::Action::GoAway) {
        log::error("HTTP/2 client connection closed while waiting for response (action={}, error={})",
                   static_cast<int>(result.action), http2::ErrorCodeName(result.errorCode));
        return false;
      }

      if (result.bytesConsumed == 0) {
        break;
      }
    }
  }

  auto iter = _streamResponses.find(streamId);
  return iter != _streamResponses.end() && iter->second.complete;
}

uint32_t TlsHttp2Client::sendRequest(std::string_view method, std::string_view path,
                                     const std::vector<std::pair<std::string, std::string>>& headers,
                                     std::string_view body) {
  uint32_t streamId = _nextStreamId;
  _nextStreamId += 2;  // Client streams are odd-numbered

  bool endStream = body.empty();

  // Send HEADERS frame
  auto err = _http2Connection->sendHeaders(
      streamId,
      [&](const http2::HeaderCallback& emit) {
        emit(":method", method);
        emit(":scheme", "https");
        emit(":authority", "localhost:" + std::to_string(_port));
        emit(":path", path);
        for (const auto& [name, value] : headers) {
          emit(name, value);
        }
      },
      endStream);

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

}  // namespace aeronet::test
