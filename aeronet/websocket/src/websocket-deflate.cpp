#include "aeronet/websocket-deflate.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/stringconv.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/websocket-compress.hpp"
#endif

namespace aeronet::websocket {

namespace {

constexpr std::string_view kPermessageDeflate = "permessage-deflate";
constexpr std::string_view kServerNoContextTakeover = "server_no_context_takeover";
constexpr std::string_view kClientNoContextTakeover = "client_no_context_takeover";
constexpr std::string_view kServerMaxWindowBits = "server_max_window_bits";
constexpr std::string_view kClientMaxWindowBits = "client_max_window_bits";

#ifdef AERONET_ENABLE_ZLIB
// Parse a single extension parameter (name=value or just name)
struct ExtensionParam {
  std::string_view name;
  std::optional<std::string_view> value;
};

[[nodiscard]] ExtensionParam ParseExtensionParam(std::string_view param) {
  const auto eqPos = param.find('=');
  if (eqPos == std::string_view::npos) {
    return {TrimOws(param), std::nullopt};
  }
  auto name = TrimOws(param.substr(0, eqPos));
  auto value = TrimOws(param.substr(eqPos + 1));
  // Remove quotes if present
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  return {name, value};
}

// Parse window bits value (8-15)
[[nodiscard]] std::optional<uint8_t> ParseWindowBits(std::string_view value) {
  int bits = 0;
  auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), bits);
  if (ec != std::errc{} || ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  if (bits < 8 || bits > 15) {
    return std::nullopt;
  }
  return static_cast<uint8_t>(bits);
}
#endif

}  // namespace

std::optional<DeflateNegotiatedParams> ParseDeflateOffer(std::string_view extensionOffer,
                                                         const DeflateConfig& serverConfig) {
#ifndef AERONET_ENABLE_ZLIB
  (void)extensionOffer;
  (void)serverConfig;
  return std::nullopt;
#else
  DeflateNegotiatedParams params;
  params.serverMaxWindowBits = serverConfig.serverMaxWindowBits;
  params.clientMaxWindowBits = serverConfig.clientMaxWindowBits;
  params.serverNoContextTakeover = serverConfig.serverNoContextTakeover;
  params.clientNoContextTakeover = serverConfig.clientNoContextTakeover;

  // Parse extension offer: "permessage-deflate; param1; param2=value; ..."
  std::size_t pos = 0;

  // First token should be the extension name
  const auto semiPos = extensionOffer.find(';');
  auto extensionName = TrimOws(extensionOffer.substr(0, semiPos));

  if (!CaseInsensitiveEqual(extensionName, kPermessageDeflate)) {
    return std::nullopt;
  }

  if (semiPos == std::string_view::npos) {
    return params;  // No parameters, use defaults
  }

  pos = semiPos + 1;

  // Parse remaining parameters
  while (pos < extensionOffer.size()) {
    const auto nextSemi = extensionOffer.find(';', pos);
    const auto paramEnd = (nextSemi == std::string_view::npos) ? extensionOffer.size() : nextSemi;

    auto paramStr = extensionOffer.substr(pos, paramEnd - pos);
    auto [name, value] = ParseExtensionParam(paramStr);

    if (CaseInsensitiveEqual(name, kServerNoContextTakeover)) {
      params.serverNoContextTakeover = true;
    } else if (CaseInsensitiveEqual(name, kClientNoContextTakeover)) {
      params.clientNoContextTakeover = true;
    } else if (CaseInsensitiveEqual(name, kServerMaxWindowBits)) {
      if (value.has_value()) {
        auto bits = ParseWindowBits(*value);
        if (!bits.has_value()) {
          return std::nullopt;  // Invalid parameter value
        }
        // Server can accept client's request if it's <= our configured value
        params.serverMaxWindowBits = std::min(*bits, serverConfig.serverMaxWindowBits);
      }
    } else if (CaseInsensitiveEqual(name, kClientMaxWindowBits)) {
      if (value.has_value()) {
        auto bits = ParseWindowBits(*value);
        if (!bits.has_value()) {
          return std::nullopt;  // Invalid parameter value
        }
        params.clientMaxWindowBits = std::min(*bits, serverConfig.clientMaxWindowBits);
      }
      // If no value, client is advertising capability; server can set the value
    }
    // Unknown parameters are ignored per RFC 7692

    pos = (nextSemi == std::string_view::npos) ? extensionOffer.size() : nextSemi + 1;
  }

  return params;
#endif
}

RawBytes BuildDeflateResponse(DeflateNegotiatedParams params) {
  RawBytes response(64);
  response.unchecked_append(std::as_bytes(std::span(kPermessageDeflate)));

  static constexpr std::byte kSemicolonSpace[] = {std::byte{';'}, std::byte{' '}};

  if (params.serverNoContextTakeover) {
    response.append(kSemicolonSpace);
    response.append(std::as_bytes(std::span(kServerNoContextTakeover)));
  }
  if (params.clientNoContextTakeover) {
    response.append(kSemicolonSpace);
    response.append(std::as_bytes(std::span(kClientNoContextTakeover)));
  }
  if (params.serverMaxWindowBits < 15) {
    response.append(kSemicolonSpace);
    response.append(std::as_bytes(std::span(kServerMaxWindowBits)));
    response.push_back(std::byte{'='});
    const auto serverWindowBitsVec = IntegralToCharVector(params.serverMaxWindowBits);
    response.append(std::as_bytes(std::span(serverWindowBitsVec)));
  }
  if (params.clientMaxWindowBits < 15) {
    response.append(kSemicolonSpace);
    response.append(std::as_bytes(std::span(kClientMaxWindowBits)));
    response.push_back(std::byte{'='});
    const auto clientWindowBitsVec = IntegralToCharVector(params.clientMaxWindowBits);
    response.append(std::as_bytes(std::span(clientWindowBitsVec)));
  }

  return response;
}

struct DeflateContext::Impl {
#ifdef AERONET_ENABLE_ZLIB
  explicit Impl(int8_t compressionLevel) : compressor(compressionLevel) {}

  WebSocketCompressor compressor;
  WebSocketDecompressor decompressor;
#else
  explicit Impl([[maybe_unused]] int8_t compressionLevel) {}
#endif
  bool deflateNoContextTakeover{false};
  bool inflateNoContextTakeover{false};
  int deflateWindowBits{15};
  int inflateWindowBits{15};
};

DeflateContext::DeflateContext(DeflateNegotiatedParams params, const DeflateConfig& config, bool isServerSide)
    : _impl(std::make_unique<Impl>(config.compressionLevel)), _minCompressSize(config.minCompressSize) {
  if (isServerSide) {
    // Server compresses using its own window bits
    _impl->deflateWindowBits = params.serverMaxWindowBits;
    // Server decompresses using client's window bits
    _impl->inflateWindowBits = params.clientMaxWindowBits;
    _impl->deflateNoContextTakeover = params.serverNoContextTakeover;
    _impl->inflateNoContextTakeover = params.clientNoContextTakeover;
  } else {
    // Client compresses using its own window bits
    _impl->deflateWindowBits = params.clientMaxWindowBits;
    // Client decompresses using server's window bits
    _impl->inflateWindowBits = params.serverMaxWindowBits;
    _impl->deflateNoContextTakeover = params.clientNoContextTakeover;
    _impl->inflateNoContextTakeover = params.serverNoContextTakeover;
  }
}

DeflateContext::~DeflateContext() = default;

bool DeflateContext::compress(std::span<const std::byte> input, RawBytes& output) {
#ifdef AERONET_ENABLE_ZLIB
  if (!_impl->compressor.compress(input, output, _impl->deflateNoContextTakeover)) {
    _lastError = _impl->compressor.lastError();
    return false;
  }
  return true;
#else
  (void)input;
  (void)output;
  _lastError = "zlib not enabled in build";
  return false;
#endif
}

bool DeflateContext::decompress(std::span<const std::byte> input, RawBytes& output, std::size_t maxDecompressedSize) {
#ifdef AERONET_ENABLE_ZLIB
  if (!_impl->decompressor.decompress(input, output, maxDecompressedSize, _impl->inflateNoContextTakeover)) {
    _lastError = _impl->decompressor.lastError();
    return false;
  }
  return true;
#else
  (void)input;
  (void)output;
  (void)maxDecompressedSize;
  _lastError = "zlib not enabled in build";
  return false;
#endif
}

}  // namespace aeronet::websocket
