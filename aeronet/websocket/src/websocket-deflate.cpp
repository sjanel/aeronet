#include "aeronet/websocket-deflate.hpp"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

#include "aeronet/decimal-writer.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-bytes.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include <algorithm>
#include <system_error>

#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/vector.hpp"
#include "websocket-compress.hpp"
#endif

namespace aeronet::websocket {

namespace {

constexpr std::string_view kPermessageDeflate = "permessage-deflate";
constexpr std::string_view kServerNoContextTakeover = "server_no_context_takeover";
constexpr std::string_view kClientNoContextTakeover = "client_no_context_takeover";
constexpr std::string_view kServerMaxWindowBits = "server_max_window_bits";
constexpr std::string_view kClientMaxWindowBits = "client_max_window_bits";

// LZ77 window sizes allowed by RFC 7692: 2^8 to 2^15 bytes.
constexpr bool IsValidWindowBits(uint8_t bits) noexcept { return 8 <= bits && bits <= 15; }

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

// Smallest LZ77 window zlib can compress raw DEFLATE data with: a 256 bytes window (8 bits) is not supported. With 9
// bits, zlib limits the match distances to 512 - 262 = 250 bytes: its output is valid for a 256 bytes window.
constexpr uint8_t kMinDeflateWindowBits = 9;

// Parse window bits value (8-15), 0 if invalid
[[nodiscard]] uint8_t ParseWindowBits(std::string_view value) {
  uint8_t bits = 0;
  auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), bits);
  if (ec != std::errc{} || ptr != value.data() + value.size() || !IsValidWindowBits(bits)) {
    bits = 0;  // Invalid value
  }
  return bits;
}
#endif

constexpr std::string_view kSemicolonSpace = "; ";

}  // namespace

void DeflateConfig::validate() const {
#ifndef AERONET_ENABLE_ZLIB
  if (enabled) {
    throw std::invalid_argument("DeflateConfig: Compression enabled but AERONET_ENABLE_ZLIB is not defined");
  }
#endif
  if (compressionLevel < 0 || compressionLevel > 9) {
    throw std::invalid_argument("DeflateConfig: compressionLevel must be between 0 and 9");
  }
  if (!IsValidWindowBits(serverMaxWindowBits)) {
    throw std::invalid_argument("DeflateConfig: serverMaxWindowBits must be between 8 and 15");
  }
  if (!IsValidWindowBits(clientMaxWindowBits)) {
    throw std::invalid_argument("DeflateConfig: clientMaxWindowBits must be between 8 and 15");
  }
  if (minCompressSize < 16U) {
    throw std::invalid_argument("DeflateConfig: minCompressSize must be at least 16 bytes");
  }
}

std::optional<DeflateNegotiatedParams> ParseDeflateOffer([[maybe_unused]] std::string_view extensionOffer,
                                                         [[maybe_unused]] const DeflateConfig& serverConfig) {
#ifndef AERONET_ENABLE_ZLIB
  return std::nullopt;
#else
  // The client window stays the default one (15) unless the client offers client_max_window_bits: the response must
  // not include it otherwise (RFC 7692 section 7.1.2.2).
  DeflateNegotiatedParams params;
  params.serverMaxWindowBits = serverConfig.serverMaxWindowBits;
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

    const auto paramStr = extensionOffer.substr(pos, paramEnd - pos);
    const auto [name, value] = ParseExtensionParam(paramStr);

    if (CaseInsensitiveEqual(name, kServerNoContextTakeover)) {
      params.serverNoContextTakeover = true;
    } else if (CaseInsensitiveEqual(name, kClientNoContextTakeover)) {
      params.clientNoContextTakeover = true;
    } else if (CaseInsensitiveEqual(name, kServerMaxWindowBits)) {
      if (value.has_value()) {
        const auto bits = ParseWindowBits(*value);
        if (bits == 0) {
          return std::nullopt;  // Invalid parameter value
        }
        // Server can accept client's request if it's <= our configured value
        params.serverMaxWindowBits = std::min(bits, serverConfig.serverMaxWindowBits);
      }
    } else if (CaseInsensitiveEqual(name, kClientMaxWindowBits)) {
      // The client accepts a limit on its window. A value tells the largest window it will use.
      params.clientMaxWindowBits = serverConfig.clientMaxWindowBits;
      if (value.has_value()) {
        const auto bits = ParseWindowBits(*value);
        if (bits == 0) {
          return std::nullopt;  // Invalid parameter value
        }
        params.clientMaxWindowBits = std::min(bits, serverConfig.clientMaxWindowBits);
      }
    }
    // Unknown parameters are ignored per RFC 7692

    pos = (nextSemi == std::string_view::npos) ? extensionOffer.size() : nextSemi + 1;
  }

  return params;
#endif
}

std::size_t ComputeDeflateResponseSize(DeflateNegotiatedParams params, uint8_t nDigitsServerMaxWindowBits,
                                       uint8_t nDigitsClientMaxWindowBits) {
  std::size_t size = kPermessageDeflate.size();
  if (params.serverNoContextTakeover) {
    size += kSemicolonSpace.size() + kServerNoContextTakeover.size();
  }
  if (params.clientNoContextTakeover) {
    size += kSemicolonSpace.size() + kClientNoContextTakeover.size();
  }
  if (params.serverMaxWindowBits < 15) {
    size += kSemicolonSpace.size() + kServerMaxWindowBits.size() + 1U + nDigitsServerMaxWindowBits;
  }
  if (params.clientMaxWindowBits < 15) {
    size += kSemicolonSpace.size() + kClientMaxWindowBits.size() + 1U + nDigitsClientMaxWindowBits;
  }
  return size;
}

char* BuildDeflateResponse(DeflateNegotiatedParams params, char* pData) {
  pData = AppendFixed<kPermessageDeflate>(pData);

  if (params.serverNoContextTakeover) {
    pData = AppendFixed<kSemicolonSpace>(pData);
    pData = AppendFixed<kServerNoContextTakeover>(pData);
  }
  if (params.clientNoContextTakeover) {
    pData = AppendFixed<kSemicolonSpace>(pData);
    pData = AppendFixed<kClientNoContextTakeover>(pData);
  }
  if (params.serverMaxWindowBits < 15) {
    pData = AppendFixed<kSemicolonSpace>(pData);
    pData = AppendFixed<kServerMaxWindowBits>(pData);
    *pData++ = '=';
    pData = WriteUInt(pData, params.serverMaxWindowBits, ndigits(params.serverMaxWindowBits));
  }
  if (params.clientMaxWindowBits < 15) {
    pData = AppendFixed<kSemicolonSpace>(pData);
    pData = AppendFixed<kClientMaxWindowBits>(pData);
    *pData++ = '=';
    pData = WriteUInt(pData, params.clientMaxWindowBits, ndigits(params.clientMaxWindowBits));
  }
  return pData;
}

struct SharedDeflateStreams::Impl {
#ifdef AERONET_ENABLE_ZLIB
  struct Compressor {
    std::unique_ptr<WebSocketCompressor> compressor;
    int8_t level;
    uint8_t windowBits;
  };

  WebSocketCompressor& compressor(int8_t level, uint8_t windowBits) {
    const auto it = std::ranges::find_if(compressors, [level, windowBits](const Compressor& entry) {
      return entry.level == level && entry.windowBits == windowBits;
    });
    if (it != compressors.end()) {
      return *it->compressor;
    }
    return *compressors.emplace_back(std::make_unique<WebSocketCompressor>(level, windowBits), level, windowBits)
                .compressor;
  }

  // One compressor per distinct configuration of the WebSocket endpoints (in practice, one or a few), created on first
  // use. Each is allocated on its own, so that its address does not change when another one is added.
  vector<Compressor> compressors;
  // Without context takeover, the window only holds the message being decompressed: the largest one decompresses the
  // data of any peer, whatever its negotiated window.
  WebSocketDecompressor decompressor{ZStreamRAII::kMaxWindowBits};
#endif
};

SharedDeflateStreams::SharedDeflateStreams() noexcept = default;
SharedDeflateStreams::SharedDeflateStreams(SharedDeflateStreams&&) noexcept = default;
SharedDeflateStreams& SharedDeflateStreams::operator=(SharedDeflateStreams&&) noexcept = default;
SharedDeflateStreams::~SharedDeflateStreams() = default;

struct DeflateContext::Impl {
  // Window bits and context takeover: own parameters when compressing, the peer's when decompressing.
#ifdef AERONET_ENABLE_ZLIB
  // An 8 bits window may be negotiated for either side: zlib cannot compress with it, 9 bits produce data valid for it.
  // A direction without context takeover uses the shared stream if any, its own one staying without zlib state.
  Impl(int8_t compressionLevel, uint8_t deflateWindowBits, uint8_t inflateWindowBits, bool deflateNoContextTakeover,
       bool inflateNoContextTakeover, SharedDeflateStreams* sharedStreams)
      : ownCompressor(compressionLevel, std::max(deflateWindowBits, kMinDeflateWindowBits)),
        ownDecompressor(inflateWindowBits),
        compressor(&ownCompressor),
        decompressor(&ownDecompressor),
        deflateNoContextTakeover(deflateNoContextTakeover),
        inflateNoContextTakeover(inflateNoContextTakeover) {
    if (sharedStreams != nullptr && (deflateNoContextTakeover || inflateNoContextTakeover)) {
      if (!sharedStreams->_impl) {
        sharedStreams->_impl = std::make_unique<SharedDeflateStreams::Impl>();
      }
      if (deflateNoContextTakeover) {
        compressor =
            &sharedStreams->_impl->compressor(compressionLevel, std::max(deflateWindowBits, kMinDeflateWindowBits));
      }
      if (inflateNoContextTakeover) {
        decompressor = &sharedStreams->_impl->decompressor;
      }
    }
  }

  WebSocketCompressor ownCompressor;
  WebSocketDecompressor ownDecompressor;
  WebSocketCompressor* compressor;
  WebSocketDecompressor* decompressor;
#else
  Impl([[maybe_unused]] int8_t compressionLevel, [[maybe_unused]] uint8_t deflateWindowBits,
       [[maybe_unused]] uint8_t inflateWindowBits, bool deflateNoContextTakeover, bool inflateNoContextTakeover,
       [[maybe_unused]] SharedDeflateStreams* sharedStreams)
      : deflateNoContextTakeover(deflateNoContextTakeover), inflateNoContextTakeover(inflateNoContextTakeover) {}
#endif
  bool deflateNoContextTakeover;
  bool inflateNoContextTakeover;
};

DeflateContext::DeflateContext(DeflateNegotiatedParams params, const DeflateConfig& config, bool isServerSide,
                               SharedDeflateStreams* sharedStreams)
    : _impl(
          isServerSide
              ? std::make_unique<Impl>(config.compressionLevel, params.serverMaxWindowBits, params.clientMaxWindowBits,
                                       params.serverNoContextTakeover, params.clientNoContextTakeover, sharedStreams)
              : std::make_unique<Impl>(config.compressionLevel, params.clientMaxWindowBits, params.serverMaxWindowBits,
                                       params.clientNoContextTakeover, params.serverNoContextTakeover, sharedStreams)),
      _minCompressSize(config.minCompressSize) {}

DeflateContext::~DeflateContext() = default;

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
const char* DeflateContext::compress([[maybe_unused]] std::span<const std::byte> input,
                                     [[maybe_unused]] RawBytes& output) {
#ifdef AERONET_ENABLE_ZLIB
  return _impl->compressor->compress(input, output, _impl->deflateNoContextTakeover);
#else
  return "zlib not enabled in build";
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
const char* DeflateContext::decompress([[maybe_unused]] std::span<const std::byte> input,
                                       [[maybe_unused]] RawBytes& output,
                                       [[maybe_unused]] std::size_t maxDecompressedSize) {
#ifdef AERONET_ENABLE_ZLIB
  return _impl->decompressor->decompress(input, output, maxDecompressedSize, _impl->inflateNoContextTakeover);
#else
  return "zlib not enabled in build";
#endif
}

}  // namespace aeronet::websocket
