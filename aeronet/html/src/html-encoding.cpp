#include "html-encoding.hpp"

#include <lexbor/core/base.h>
#include <lexbor/core/types.h>
#include <lexbor/encoding/base.h>
#include <lexbor/encoding/const.h>
#include <lexbor/encoding/encoding.h>
#include <lexbor/html/encoding.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "aeronet/header-value-parameters.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "html-node-ops.hpp"
#include "lexbor-utils.hpp"

namespace aeronet::internal {

namespace {

// The <meta> prescan of the encoding sniffing algorithm only reads the start of the document.
constexpr std::size_t kPrescanSize = 1024;

// The input is given to the lexbor decoders in chunks small enough to never fill their output buffer: lexbor (3.0.1)
// single-byte decoders lose the byte whose code point does not fit. A decoder produces at most one code point per byte
// of a chunk, plus a few for a sequence started in the previous chunk.
constexpr std::size_t kDecodeInputChunkSize = 1024;
constexpr std::size_t kDecodeBufferSize = kDecodeInputChunkSize + 8;

using DecodeBuffer = std::array<lxb_codepoint_t, kDecodeBufferSize>;

const lxb_char_t* Bytes(std::string_view text) noexcept { return reinterpret_cast<const lxb_char_t*>(text.data()); }

std::string_view CharsetParameter(std::string_view contentType) noexcept {
  http::HeaderParametersIterator parameters(contentType);
  while (parameters.hasNext()) {
    const http::HeaderParameter parameter = parameters.next();
    if (CaseInsensitiveEqual(parameter.name, "charset")) {
      return parameter.value;
    }
  }
  return {};
}

// The encoding declared by a <meta> element at the start of 'html' (the prescan of the HTML standard), or nullptr.
const lxb_encoding_data_t* PrescanEncoding(std::string_view html) {
  lxb_html_encoding_t prescan{};
  const lxb_status_t status = lxb_html_encoding_init(&prescan);
  const lxb_char_t* pName = nullptr;
  std::size_t nameSize = 0;
  if (status == LXB_STATUS_OK) {
    pName =
        lxb_html_encoding_prescan(&prescan, Bytes(html), Bytes(html) + std::min(html.size(), kPrescanSize), &nameSize);
  }
  // Also after a failed init, which can leave its first array allocated. The name points into 'html' or into static
  // data of lexbor, not into the prescan state.
  lxb_html_encoding_destroy(&prescan, false);
  CheckStatus(status);
  return pName == nullptr ? nullptr : lxb_encoding_data_prescan_validate(pName, nameSize);
}

// Decodes 'text' with the decoder of 'decode' (see kDecodeInputChunkSize), calling sink(codepoints) after each chunk.
// Stops at the first LXB_STATUS_ERROR, which a decoder only returns without replacement code point.
template <class Sink>
lxb_status_t Decode(lxb_encoding_decode_t& decode, std::string_view text, Sink sink) {
  const lxb_char_t* pData = Bytes(text);
  const lxb_char_t* const pEnd = pData + text.size();
  lxb_status_t status = LXB_STATUS_OK;
  while (pData != pEnd && status != LXB_STATUS_ERROR) {
    const lxb_char_t* pChunkEnd = pData + std::min(static_cast<std::size_t>(pEnd - pData), kDecodeInputChunkSize);
    status = decode.encoding_data->decode(&decode, &pData, pChunkEnd);
    assert(status != LXB_STATUS_SMALL_BUFFER);
    sink(std::span<const lxb_codepoint_t>(decode.buffer_out, decode.buffer_used));
    decode.buffer_used = 0;
  }
  return status;
}

// The size of the ASCII start of 'text', read 8 bytes at a time.
std::size_t AsciiPrefixSize(std::string_view text) noexcept {
  static constexpr uint64_t kHighBits = 0x8080808080808080ULL;
  std::size_t pos = 0;
  for (; pos + sizeof(uint64_t) <= text.size(); pos += sizeof(uint64_t)) {
    uint64_t word;
    std::memcpy(&word, text.data() + pos, sizeof(word));
    if ((word & kHighBits) != 0) {
      break;
    }
  }
  while (pos < text.size() && static_cast<unsigned char>(text[pos]) < 0x80U) {
    ++pos;
  }
  return pos;
}

// Without a replacement code point, the lexbor decoders stop at the first invalid sequence with LXB_STATUS_ERROR.
bool IsValidUtf8(std::string_view text) {
  DecodeBuffer codepoints;
  lxb_encoding_decode_t decode{};
  static_cast<void>(
      lxb_encoding_decode_init(&decode, lxb_encoding_data(LXB_ENCODING_UTF_8), codepoints.data(), codepoints.size()));
  const lxb_status_t status = Decode(decode, text, [](std::span<const lxb_codepoint_t>) {});
  // finish() reports a sequence truncated by the end of the text.
  return status != LXB_STATUS_ERROR && lxb_encoding_decode_finish(&decode) == LXB_STATUS_OK;
}

// Writes the UTF-8 encoding of codepoints (Unicode scalar values) at the end of out.
void AppendUtf8(std::span<const lxb_codepoint_t> codepoints, RawChars& out) {
  static constexpr std::size_t kMaxUtf8Size = 4;
  out.ensureAvailableCapacityExponential(codepoints.size() * kMaxUtf8Size);
  char* pOut = out.end();
  for (const lxb_codepoint_t codepoint : codepoints) {
    if (codepoint < 0x80U) {
      *pOut++ = static_cast<char>(codepoint);
    } else if (codepoint < 0x800U) {
      *pOut++ = static_cast<char>(0xC0U | (codepoint >> 6U));
      *pOut++ = static_cast<char>(0x80U | (codepoint & 0x3FU));
    } else if (codepoint < 0x10000U) {
      *pOut++ = static_cast<char>(0xE0U | (codepoint >> 12U));
      *pOut++ = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
      *pOut++ = static_cast<char>(0x80U | (codepoint & 0x3FU));
    } else {
      *pOut++ = static_cast<char>(0xF0U | (codepoint >> 18U));
      *pOut++ = static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU));
      *pOut++ = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
      *pOut++ = static_cast<char>(0x80U | (codepoint & 0x3FU));
    }
  }
  out.setEnd(pOut);
}

// Decodes html from pEncoding to UTF-8 into out, whose previous contents are discarded, invalid sequences being
// replaced by U+FFFD.
void DecodeToUtf8(std::string_view html, const lxb_encoding_data_t* pEncoding, RawChars& out) {
  static constexpr lxb_codepoint_t kReplacement = LXB_ENCODING_REPLACEMENT_CODEPOINT;
  DecodeBuffer codepoints;
  lxb_encoding_decode_t decode{};
  static_cast<void>(lxb_encoding_decode_init(&decode, pEncoding, codepoints.data(), codepoints.size()));
  static_cast<void>(lxb_encoding_decode_replace_set(&decode, &kReplacement, 1));
  out.clear();
  // Documents in a legacy encoding are mostly ASCII: about one UTF-8 byte per input byte.
  out.reserve(html.size());
  static_cast<void>(Decode(decode, html, [&out](std::span<const lxb_codepoint_t> chunk) { AppendUtf8(chunk, out); }));
  // A sequence truncated by the end of the input becomes one more replacement character.
  static_cast<void>(lxb_encoding_decode_finish(&decode));
  AppendUtf8({codepoints.data(), lxb_encoding_decode_buf_used(&decode)}, out);
}

// The encoding of the label 'label' (WHATWG Encoding "get an encoding"), or nullptr.
const lxb_encoding_data_t* EncodingOf(std::string_view label) noexcept {
  return lxb_encoding_data_by_pre_name(Bytes(label), label.size());
}

}  // namespace

std::string_view DecodeScript(std::string_view bytes, std::string_view contentType, std::string_view charset,
                              std::string_view documentEncoding, RawChars& buffer) {
  const lxb_encoding_data_t* pEncoding;
  const lxb_encoding_t bomEncoding = lxb_encoding_bom_sniff(Bytes(bytes), bytes.size());
  if (bomEncoding != LXB_ENCODING_DEFAULT) {
    bytes.remove_prefix(bomEncoding == LXB_ENCODING_UTF_8 ? 3U : 2U);
    pEncoding = lxb_encoding_data(bomEncoding);
  } else {
    pEncoding = EncodingOf(CharsetParameter(contentType));
    for (const std::string_view fallback : {charset, documentEncoding}) {
      if (pEncoding == nullptr) {
        pEncoding = EncodingOf(fallback);
      }
    }
    if (pEncoding == nullptr) {
      pEncoding = lxb_encoding_data(LXB_ENCODING_UTF_8);
    }
  }
  if (pEncoding->encoding == LXB_ENCODING_UTF_8) {
    return DecodeUtf8(bytes, buffer);
  }
  DecodeToUtf8(bytes, pEncoding, buffer);
  return {buffer.data(), buffer.size()};
}

std::string_view DecodeUtf8(std::string_view bytes, RawChars& buffer) {
  // Texts are mostly ASCII: only the rest is validated by the decoder.
  if (IsValidUtf8(bytes.substr(AsciiPrefixSize(bytes)))) {
    return bytes;
  }
  DecodeToUtf8(bytes, lxb_encoding_data(LXB_ENCODING_UTF_8), buffer);
  return {buffer.data(), buffer.size()};
}

Utf8Html DecodeHtml(std::string_view html, std::string_view contentType, RawChars& buffer) {
  const lxb_encoding_data_t* pEncoding;
  // A byte order mark wins over any declaration, and is not part of the document.
  const lxb_encoding_t bomEncoding = lxb_encoding_bom_sniff(Bytes(html), html.size());
  if (bomEncoding != LXB_ENCODING_DEFAULT) {
    html.remove_prefix(bomEncoding == LXB_ENCODING_UTF_8 ? 3U : 2U);
    pEncoding = lxb_encoding_data(bomEncoding);
  } else {
    const std::string_view charset = CharsetParameter(contentType);
    pEncoding = lxb_encoding_data_by_pre_name(Bytes(charset), charset.size());
    if (pEncoding == nullptr) {
      pEncoding = PrescanEncoding(html);
    }
    if (pEncoding == nullptr) {
      pEncoding = lxb_encoding_data(IsValidUtf8(html) ? LXB_ENCODING_UTF_8 : LXB_ENCODING_WINDOWS_1252);
    }
  }
  if (pEncoding->encoding == LXB_ENCODING_UTF_8) {
    return {html, pEncoding};
  }
  DecodeToUtf8(html, pEncoding, buffer);
  return {std::string_view(buffer.data(), buffer.size()), pEncoding};
}

}  // namespace aeronet::internal
