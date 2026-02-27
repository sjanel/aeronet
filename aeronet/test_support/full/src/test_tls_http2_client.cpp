#include "aeronet/test_tls_http2_client.hpp"

#include <poll.h>

#include <algorithm>
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
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/log.hpp"
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
  headers.emplace_back(http::ContentType, std::string(contentType));
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

    _pendingInput.insert(_pendingInput.end(), reinterpret_cast<const std::byte*>(data.data()),
                         reinterpret_cast<const std::byte*>(data.data() + data.size()));

    // Process through HTTP/2 connection
    for (;;) {
      std::span<const std::byte> inputData(_pendingInput.data() + _pendingOffset,
                                           _pendingInput.size() - _pendingOffset);
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
          _pendingInput.erase(_pendingInput.begin(),
                              _pendingInput.begin() + static_cast<std::ptrdiff_t>(_pendingOffset));
          _pendingOffset = 0;
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

    _pendingInput.insert(_pendingInput.end(), reinterpret_cast<const std::byte*>(data.data()),
                         reinterpret_cast<const std::byte*>(data.data() + data.size()));

    for (;;) {
      std::span<const std::byte> inputData(_pendingInput.data() + _pendingOffset,
                                           _pendingInput.size() - _pendingOffset);
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
          _pendingInput.erase(_pendingInput.begin(),
                              _pendingInput.begin() + static_cast<std::ptrdiff_t>(_pendingOffset));
          _pendingOffset = 0;
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

  return true;
}

std::vector<std::byte> TlsHttp2Client::receiveTunnelData(uint32_t streamId, std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  std::vector<std::byte> result;

  std::array<char, 32U << 10> buffer{};
  while (std::chrono::steady_clock::now() < deadline) {
    auto iter = _streamResponses.find(streamId);
    if (iter != _streamResponses.end() && !iter->second.response.body.empty()) {
      auto& body = iter->second.response.body;
      result.insert(result.end(), reinterpret_cast<const std::byte*>(body.data()),
                    reinterpret_cast<const std::byte*>(body.data() + body.size()));
      body.clear();  // Consume the data
      return result;
    }

    if (iter != _streamResponses.end() && iter->second.complete) {
      return result;  // Stream closed
    }

    // Read and process more frames
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

    _pendingInput.insert(_pendingInput.end(), reinterpret_cast<const std::byte*>(data.data()),
                         reinterpret_cast<const std::byte*>(data.data() + data.size()));

    for (;;) {
      std::span<const std::byte> inputData(_pendingInput.data() + _pendingOffset,
                                           _pendingInput.size() - _pendingOffset);
      if (inputData.empty()) {
        break;
      }

      auto result_process = _http2Connection->processInput(inputData);

      if (result_process.bytesConsumed > 0) {
        _pendingOffset += result_process.bytesConsumed;
        if (_pendingOffset == _pendingInput.size()) {
          _pendingInput.clear();
          _pendingOffset = 0;
        } else if (_pendingOffset > (64UL * 1024UL)) {
          _pendingInput.erase(_pendingInput.begin(),
                              _pendingInput.begin() + static_cast<std::ptrdiff_t>(_pendingOffset));
          _pendingOffset = 0;
        }
      }

      if (_http2Connection->hasPendingOutput()) {
        auto output = _http2Connection->getPendingOutput();
        if (!writeAll(output)) {
          break;
        }
        _http2Connection->onOutputWritten(output.size());
      }

      if (result_process.action == http2::Http2Connection::ProcessResult::Action::Error ||
          result_process.action == http2::Http2Connection::ProcessResult::Action::Closed ||
          result_process.action == http2::Http2Connection::ProcessResult::Action::GoAway) {
        break;
      }

      if (result_process.bytesConsumed == 0) {
        break;
      }
    }
  }

  return result;
}

}  // namespace aeronet::test
