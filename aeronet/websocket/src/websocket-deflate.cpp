#include "aeronet/websocket-deflate.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

#include "aeronet/decimal-writer.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/websocket-deflate-context.hpp"
#include "aeronet/websocket-shared-deflate-streams.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include <algorithm>
#include <system_error>

#include "aeronet/header-value-parameters.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/websocket-compress.hpp"
#include "aeronet/zlib-stream-raii.hpp"
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
// Smallest LZ77 window zlib can compress raw DEFLATE data with: a 256 bytes window (8 bits) is not supported. With 9
// bits, zlib limits the match distances to 512 - 262 = 250 bytes: its output is valid for a 256 bytes window.
constexpr uint8_t kMinDeflateWindowBits = 9;

// Window bits to compress with: an 8 bits window may be negotiated for either side, 9 bits produce data valid for it.
constexpr uint8_t DeflateWindowBits(DeflateNegotiatedParams params, bool isServerSide) {
  return std::max(isServerSide ? params.serverMaxWindowBits : params.clientMaxWindowBits, kMinDeflateWindowBits);
}

// Parse window bits value (8-15), a decimal integer without leading zeroes (RFC 7692 §7.1.2), 0 if invalid
[[nodiscard]] uint8_t ParseWindowBits(std::string_view value) {
  uint8_t bits = 0;
  const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), bits);
  if (ec != std::errc{} || ptr != value.data() + value.size() || value.front() == '0' || !IsValidWindowBits(bits)) {
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

DeflateNegotiatedParams ParseDeflateOffer([[maybe_unused]] std::string_view extensionOffer,
                                          [[maybe_unused]] const DeflateConfig& serverConfig) {
  DeflateNegotiatedParams params;
#ifdef AERONET_ENABLE_ZLIB
  // First token should be the extension name
  const auto semiPos = extensionOffer.find(';');
  std::string_view extensionName = TrimOws(extensionOffer.substr(0, semiPos));

  if (!CaseInsensitiveEqual(extensionName, kPermessageDeflate)) {
    return params;
  }

  // The client window stays the default one (15) unless the client offers client_max_window_bits: the response must
  // not include it otherwise (RFC 7692 section 7.1.2.2).
  params.serverMaxWindowBits = serverConfig.serverMaxWindowBits;
  params.serverNoContextTakeover = serverConfig.serverNoContextTakeover;
  params.clientNoContextTakeover = serverConfig.clientNoContextTakeover;

  if (semiPos == std::string_view::npos) {
    return params;  // No parameters, use defaults
  }

  // Parse extension offer: "permessage-deflate; param1; param2=value; ...". The offer is declined when a parameter is
  // unknown, has an invalid value or is repeated (RFC 7692 §7): the next offer of the client, if any, is considered.
  enum : uint8_t {
    kSeenServerNoContextTakeover = 1U << 0U,
    kSeenClientNoContextTakeover = 1U << 1U,
    kSeenServerMaxWindowBits = 1U << 2U,
    kSeenClientMaxWindowBits = 1U << 3U,
  };
  uint8_t seenParams = 0;
  // Tells whether the parameter was not seen yet, and records it.
  const auto firstOccurrence = [&seenParams](uint8_t paramBit) {
    const bool first = (seenParams & paramBit) == 0;
    seenParams |= paramBit;
    return first;
  };

  std::size_t pos = semiPos + 1;
  for (;;) {
    const auto nextSemi = extensionOffer.find(';', pos);
    const auto paramEnd = (nextSemi == std::string_view::npos) ? extensionOffer.size() : nextSemi;
    // A parameter without value (no '=') is valid in an extension offer (RFC 6455 §9.1).
    const http::HeaderParameter param = http::ParseHeaderParameter(extensionOffer.substr(pos, paramEnd - pos));

    bool valid = false;
    if (CaseInsensitiveEqual(param.name, kServerNoContextTakeover)) {
      // A parameter without value (RFC 7692 §7.1.1.1)
      valid = !param.hasValue() && firstOccurrence(kSeenServerNoContextTakeover);
      params.serverNoContextTakeover = true;
    } else if (CaseInsensitiveEqual(param.name, kClientNoContextTakeover)) {
      // A parameter without value (RFC 7692 §7.1.1.2)
      valid = !param.hasValue() && firstOccurrence(kSeenClientNoContextTakeover);
      params.clientNoContextTakeover = true;
    } else if (CaseInsensitiveEqual(param.name, kServerMaxWindowBits)) {
      // The largest window the server may use, a value is required in an offer (RFC 7692 §7.1.2.1)
      const auto bits = param.hasValue() ? ParseWindowBits(param.value) : uint8_t{0};
      valid = bits != 0 && firstOccurrence(kSeenServerMaxWindowBits);
      // Server can accept client's request if it's <= our configured value
      params.serverMaxWindowBits = std::min(bits, serverConfig.serverMaxWindowBits);
    } else if (CaseInsensitiveEqual(param.name, kClientMaxWindowBits)) {
      // The client accepts a limit on its window. A value, optional, tells the largest window it will use
      // (RFC 7692 §7.1.2.2).
      const auto bits = param.hasValue() ? ParseWindowBits(param.value) : serverConfig.clientMaxWindowBits;
      valid = bits != 0 && firstOccurrence(kSeenClientMaxWindowBits);
      params.clientMaxWindowBits = std::min(bits, serverConfig.clientMaxWindowBits);
    }
    if (!valid) {
      params.setEmpty();  // unknown or repeated parameter, or invalid value
      return params;
    }
    if (nextSemi == std::string_view::npos) {
      break;
    }
    pos = nextSemi + 1;
  }
#endif
  return params;
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

#ifdef AERONET_ENABLE_ZLIB
// A direction without context takeover uses the shared stream if any, its own one staying without zlib state.
DeflateContext::DeflateContext(DeflateNegotiatedParams params, const DeflateConfig& config, bool isServerSide,
                               SharedDeflateStreams* sharedStreams)
    : _ownCompressor(config.compressionLevel, DeflateWindowBits(params, isServerSide)),
      _ownDecompressor(isServerSide ? params.clientMaxWindowBits : params.serverMaxWindowBits),
      _compressor(&_ownCompressor),
      _decompressor(&_ownDecompressor),
      _deflateNoContextTakeover(isServerSide ? params.serverNoContextTakeover : params.clientNoContextTakeover),
      _inflateNoContextTakeover(isServerSide ? params.clientNoContextTakeover : params.serverNoContextTakeover),
      _minCompressSize(config.minCompressSize) {
  if (sharedStreams != nullptr && (_deflateNoContextTakeover || _inflateNoContextTakeover)) {
    if (!sharedStreams->_impl) {
      sharedStreams->_impl = std::make_unique<SharedDeflateStreams::Impl>();
    }
    if (_deflateNoContextTakeover) {
      _compressor = &sharedStreams->_impl->compressor(config.compressionLevel, DeflateWindowBits(params, isServerSide));
    }
    if (_inflateNoContextTakeover) {
      _decompressor = &sharedStreams->_impl->decompressor;
    }
  }
}
#else
DeflateContext::DeflateContext([[maybe_unused]] DeflateNegotiatedParams params, const DeflateConfig& config,
                               [[maybe_unused]] bool isServerSide, [[maybe_unused]] SharedDeflateStreams* sharedStreams)
    : _minCompressSize(config.minCompressSize) {}
#endif

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
const char* DeflateContext::compress([[maybe_unused]] std::span<const std::byte> input,
                                     [[maybe_unused]] RawBytes& output) {
#ifdef AERONET_ENABLE_ZLIB
  return _compressor->compress(input, output, _deflateNoContextTakeover);
#else
  return "zlib not enabled in build";
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
const char* DeflateContext::decompress([[maybe_unused]] std::span<const std::byte> input,
                                       [[maybe_unused]] RawBytes& output,
                                       [[maybe_unused]] std::size_t maxDecompressedSize) {
#ifdef AERONET_ENABLE_ZLIB
  return _decompressor->decompress(input, output, maxDecompressedSize, _inflateNoContextTakeover);
#else
  return "zlib not enabled in build";
#endif
}

}  // namespace aeronet::websocket
