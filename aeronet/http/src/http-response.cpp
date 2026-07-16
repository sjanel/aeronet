#include "aeronet/http-response.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include "aeronet/header-write.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message-common.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

namespace {

// Date header will always be present at headersStartPos.
constexpr std::size_t kDateHeaderLenWithCRLF = http::HeaderSize(http::Date.size(), RFC7231DateStrLen);

// Initial size of the HttpMessage internal buffer, including the status line, Date header and DoubleCRLF.
constexpr std::size_t kHttpResponseInitialSize =
    HttpResponse::kReasonBeg + kDateHeaderLenWithCRLF + http::DoubleCRLF.size();

static_assert(kHttpResponseInitialSize <= HttpResponse::kHttpResponseMinInitialCapacity,
              "Initial size should be less than or equal to min initial capacity");

constexpr auto kInitialBodyStart = kHttpResponseInitialSize;

constexpr void InitData(char* data) {
  data[http::HTTP10Sv.size()] = ' ';          // SP after the HTTP version
  data[HttpResponse::kReasonBeg - 1U] = ' ';  // mandatory SP before the reason-phrase (RFC 9112 §4)
#ifndef NDEBUG
  // In debug, this allows for easier inspection of the response data before finalization.
  // In release, it's not needed because the final HTTP version and date will be written at finalization step.
  http::HTTP_1_1.writeFull(data);
  WriteCRLFDateHeader(SysClock::now(), data + HttpResponse::kReasonBeg);
#endif
  // Set last, so the debug pre-write above lands on this byte;
  // the marker must win so hasReason() stays correct before finalization (it is overwritten by the
  // status-line CRLF at finalization anyway).
  data[HttpResponse::kReasonBeg] = '\n';  // marker for no reason
}

}  // namespace

HttpResponse::HttpResponse(http::StatusCode code, std::string_view body, std::string_view contentType)
    : HttpMessage(kHttpResponseInitialSize +
                  NeededBodyHeadersSize(body.size(), CheckContentType(body.empty(), contentType).size()) +
                  body.size()) {
  InitData(_data.data());
  status(code);
  setHeadersStartPosNoCheck(kReasonBeg + kDateHeaderLenWithCRLF);
  if (body.empty()) {
    setBodyStartPosNoCheck(kInitialBodyStart);
    Copy(http::DoubleCRLF, _data.data() + kInitialBodyStart - http::DoubleCRLF.size());
    _data.setSize(kInitialBodyStart);
  } else {
    char* insertPtr =
        WriteCRLFHeader(http::ContentType, contentType, _data.data() + kReasonBeg + kDateHeaderLenWithCRLF);
    insertPtr = WriteCRLFHeader(http::ContentLength, body.size(), insertPtr);
    insertPtr = Append(http::DoubleCRLF, insertPtr);
    const auto bodyStartPos = static_cast<std::uint64_t>(insertPtr - _data.data());
    setBodyStartPos(bodyStartPos);
    Copy(body, insertPtr);
    _data.setSize(bodyStartPos + body.size());
  }
}

HttpResponse::HttpResponse(std::size_t additionalCapacity, http::StatusCode code)
    : HttpMessage(kHttpResponseInitialSize + additionalCapacity) {
  InitData(_data.data());
  status(code);
  setHeadersStartPosNoCheck(kReasonBeg + kDateHeaderLenWithCRLF);
  setBodyStartPosNoCheck(kInitialBodyStart);
  Copy(http::DoubleCRLF, _data.data() + kInitialBodyStart - http::DoubleCRLF.size());
  _data.setSize(kInitialBodyStart);
}

HttpResponse::HttpResponse(std::size_t additionalCapacity, http::StatusCode code, std::string_view concatenatedHeaders,
                           std::string_view body, std::string_view contentType, Check check)
    : HttpMessage(kHttpResponseInitialSize + concatenatedHeaders.size() +
                  NeededBodyHeadersSize(body.size(), CheckContentType(body.empty(), contentType).size()) + body.size() +
                  additionalCapacity) {
  if (check == Check::Yes) {
    CheckConcatenatedHeaders(concatenatedHeaders);
  }
  InitData(_data.data());
  status(code);
  setHeadersStartPosNoCheck(kReasonBeg + kDateHeaderLenWithCRLF);
  std::size_t bodyStartPos = kInitialBodyStart - http::CRLF.size();
  if (!concatenatedHeaders.empty()) {
    char* insertPtr = _data.data() + kHttpResponseInitialSize - http::DoubleCRLF.size();
    insertPtr = Append(http::CRLF, insertPtr);
    Copy(concatenatedHeaders, insertPtr);
    bodyStartPos += concatenatedHeaders.size();
  }
  if (body.empty()) {
    bodyStartPos += http::CRLF.size();
  } else {
    char* insertPtr = WriteCRLFHeader(http::ContentType, contentType, _data.data() + bodyStartPos - http::CRLF.size());
    insertPtr = WriteCRLFHeader(http::ContentLength, body.size(), insertPtr);
    bodyStartPos = static_cast<std::uint64_t>(insertPtr + http::DoubleCRLF.size() - _data.data());
    Copy(body, insertPtr + http::DoubleCRLF.size());
  }
  Copy(http::DoubleCRLF, _data.data() + bodyStartPos - http::DoubleCRLF.size());
  setBodyStartPos(bodyStartPos);
  _data.setSize(bodyStartPos + body.size());
}

HttpResponse& HttpResponse::status(http::StatusCode statusCode) & {
  if (statusCode < 100 || statusCode > 999) [[unlikely]] {
    throw std::invalid_argument("Invalid HTTP status code, should be 3 digits");
  }
  write3(_data.data() + kStatusCodeBeg, statusCode);
  return *this;
}

HttpResponse& HttpResponse::reason(std::string_view newReason) & {
  if (newReason.size() > kMaxReasonLength) {
    log::warn("Provided reason is too long ({} bytes), truncating it to {} bytes", newReason.length(),
              kMaxReasonLength);
    newReason.remove_suffix(newReason.size() - kMaxReasonLength);
  }
  const auto oldReasonSz = reasonLength();

  if (newReason.size() == oldReasonSz) {
    // optimization: same length, just overwrite the old reason
    Copy(newReason, _data.data() + kReasonBeg);
    return *this;
  }

  const int32_t diff = static_cast<int32_t>(newReason.size()) - static_cast<int32_t>(oldReasonSz);

  _data.ensureAvailableCapacityExponential(diff);
  // The mandatory SP that separates the status code from the reason-phrase (kReasonBeg - 1) is always
  // present, so only the reason characters themselves are inserted/removed: shift the [reason-end, end)
  // tail by `diff`. For an empty reason the reason region is itself empty (reason-end == kReasonBeg).
  char* reasonEnd = _data.data() + kReasonBeg + oldReasonSz;

  std::memmove(reasonEnd + diff, reasonEnd, _data.size() - (kReasonBeg + oldReasonSz));

  adjustHeadersAndBodyStart(diff);
  if (newReason.empty()) {
    _data[kReasonBeg] = '\n';  // marker for empty reason (overwritten by the status-line CRLF at finalization)
  } else {
    Copy(newReason, _data.data() + kReasonBeg);
  }
  _data.adjustSize(diff);
  return *this;
}

}  // namespace aeronet