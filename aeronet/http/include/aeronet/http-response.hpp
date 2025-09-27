#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "http-constants.hpp"
#include "http-status-code.hpp"
#include "raw-chars.hpp"
#include "simple-charconv.hpp"

namespace aeronet {

// -----------------------------------------------------------------------------
// HttpResponse
// -----------------------------------------------------------------------------
// A contiguous single-buffer HTTP/1.x response builder focused on minimal
// allocations and cache-friendly writes.
//
// Memory Layout (before finalize):
//   [HTTP/1.x SP status-code [SP reason] CRLF][CRLF][CRLF]  (DoubleCRLF sentinel)
//   ^             ^             ^           ^   ^
//   |             |             |           |   +-- part of DoubleCRLF
//   |             |             |           +------ end of status/optional reason line
//   |             |             +-- beginning of reason
//   |             +-- beginning of status code
//   +-- start
//
// After headers are appended:
//   Status/Reason CRLF (CRLF HeaderName ": " Value)* CRLF CRLF [Body]
//
// Header Insertion Strategy:
//   Each user header is inserted as: CRLF + name + ": " + value (no trailing CRLF).
//   The leading CRLF acts as the line terminator for either the status line (first
//   header inserted) or the previous header. The final DoubleCRLF already present
//   at construction terminates the header block. This lets us append headers by
//   shifting only the tail (DoubleCRLF + body) once per insertion.
//
//   appendHeader():
//     - O(T) memmove of tail where T = size(DoubleCRLF + current body), no scan of
//       existing headers (fast path). Allows duplicates intentionally.
//
//   header():
//     - Linear scan of current header region to find existing key at line starts
//       (recognised by preceding CRLF). If found, value replaced in-place adjusting
//       buffer via single memmove for size delta. If not found, falls back to append.
//     - Because of the scan it is less efficient than appendHeader(). Prefer
//       appendHeader() when duplicates are acceptable or order-only semantics matter.
//
// Mutators & Finalization:
//   statusCode(), reason(), body(), appendHeader(), header() may be called in any
//   order prior to finalizeAndGetFullTextResponse(). finalize* injects reserved
//   headers (Content-Length if body non-empty, Date, Connection) every time it is
//   called; therefore call it exactly once. Post-finalization mutation is NOT
//   supported and will produce duplicated reserved headers.
//
// Reserved Headers (user cannot set): Date, Connection, Content-Length,
//   Transfer-Encoding, Trailer, Upgrade, TE.
//
// Complexity Summary:
//   - statusCode(): O(1)
//   - reason(): O(size of tail - adjusts headers/body offsets)
//   - body(): O(delta) for copy; may reallocate
//   - appendHeader(): O(bodyLen) for memmove of tail
//   - header(): O(totalHeaderBytes) scan + O(bodyLen) memmove if size delta
//
// Safety & Assumptions:
//   - Not thread-safe.
//   - Throws std::bad_alloc on growth failure.
//   - Assumes ASCII header names; no validation performed.
// -----------------------------------------------------------------------------
class HttpResponse {
 private:
  static constexpr std::size_t kHttp1VersionLen = http::HTTP10.size();  // "HTTP/x.y"
  static constexpr std::size_t kStatusCodeBeg = kHttp1VersionLen + 1;   // index of first status code digit
  static constexpr std::size_t kReasonBeg = kStatusCodeBeg + 3 + 1;     // index of first reason phrase character

 public:
  explicit HttpResponse(http::StatusCode code = 200, std::string_view reason = {});

  // Replaces the status code. Must be a 3 digits integer.
  HttpResponse& statusCode(http::StatusCode statusCode) & noexcept {
    assert(statusCode >= 100 && statusCode < 1000);
    write3(_data.data() + kStatusCodeBeg, statusCode);
    return *this;
  }

  // Replaces the status code. Must be a 3 digits integer.
  HttpResponse&& statusCode(http::StatusCode statusCode) && noexcept {
    assert(statusCode >= 100 && statusCode < 1000);
    write3(_data.data() + kStatusCodeBeg, statusCode);
    return std::move(*this);
  }

  // Get the current status code stored in this HttpResponse.
  [[nodiscard]] http::StatusCode statusCode() const noexcept {
    return static_cast<http::StatusCode>(read3(_data.data() + kStatusCodeBeg));
  }

  // Sets or replace the reason phrase for this instance.
  // Inserting empty reason is allowed.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& reason(std::string_view reason) & {
    setReason(reason);
    return *this;
  }

  // Sets or replace the reason phrase for this instance.
  // Inserting empty reason is allowed.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& reason(std::string_view reason) && {
    setReason(reason);
    return std::move(*this);
  }

  // Get the current reason stored in this HttpResponse.
  [[nodiscard]] std::string_view reason() const noexcept { return {_data.data() + kReasonBeg, reasonLen()}; }

  // Assigns the given body to this HttpResponse.
  // Empty body is allowed.
  // Body referencing internal memory of this HttpResponse is allowed as well.
  // Example:
  //   HttpResponse resp(404);
  //   resp.reason("Not Found").body(resp.reason()); // OK
  HttpResponse& body(std::string_view body) & {
    setBody(body);
    return *this;
  }

  // Assigns the given body to this HttpResponse.
  // Empty body is allowed.
  // Body referencing internal memory of this HttpResponse is allowed as well.
  // Example:
  //   HttpResponse resp(404);
  //   resp.reason("Not Found").body(resp.reason()); // OK
  HttpResponse&& body(std::string_view body) && {
    setBody(body);
    return std::move(*this);
  }

  [[nodiscard]] std::string_view body() const noexcept { return {_data.data() + _bodyStartPos, bodyLen()}; }

  // Inserts or replaces the Content-Type header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& contentType(std::string_view src) & { return header(http::ContentType, src); }

  // Inserts or replaces the Content-Type header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& contentType(std::string_view src) && {
    header(http::ContentType, src);
    return std::move(*this);
  }

  // Inserts or replaces the Location header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& location(std::string_view src) & { return header(http::Location, src); }

  // Inserts or replaces the Location header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& location(std::string_view src) && {
    header(http::Location, src);
    return std::move(*this);
  }

  // appendHeader: Append a header line (duplicates allowed, fastest path).
  // No scan over existing headers; simply shifts DoubleCRLF/body tail and
  // copies the new header fragment. Prefer this when duplicates are OK or
  // when constructing headers once.
  // Do not insert any reserved header (for which IsReservedHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& appendHeader(std::string_view key, std::string_view value) & {
    assert(!IsReservedHeader(key));
    appendHeaderUnchecked(key, value);
    return *this;
  }

  // appendHeader: Append a header line (duplicates allowed, fastest path).
  // No scan over existing headers; simply shifts DoubleCRLF/body tail and
  // copies the new header fragment. Prefer this when duplicates are OK or
  // when constructing headers once.
  // Do not insert any reserved header (for which IsReservedHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& appendHeader(std::string_view key, std::string_view value) && {
    assert(!IsReservedHeader(key));
    appendHeaderUnchecked(key, value);
    return std::move(*this);
  }

  // header: Set or replace a header value ensuring at most one instance.
  // This performs a linear scan to find an existing key (slower than appendHeader()).
  // If not found, it falls back to appendHeader().
  // Use only when you must guarantee uniqueness; otherwise prefer appendHeader().
  // Do not insert any reserved header (for which IsReservedHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& header(std::string_view key, std::string_view value) & {
    assert(!IsReservedHeader(key));
    setHeader(key, value);
    return *this;
  }

  // header: Set or replace a header value ensuring at most one instance.
  // This performs a linear scan to find an existing key (slower than appendHeader()).
  // If not found, it falls back to appendHeader().
  // Use only when you must guarantee uniqueness; otherwise prefer appendHeader().
  // Do not insert any reserved header (for which IsReservedHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& header(std::string_view key, std::string_view value) && {
    assert(!IsReservedHeader(key));
    setHeader(key, value);
    return std::move(*this);
  }

  // Centralized rule for headers the user may not set directly (normal or streaming path).
  // These are either automatically emitted (Date, Content-Length, Connection, Transfer-Encoding) or
  // would create ambiguous / unsupported semantics if user-supplied before dedicated feature support
  // (Trailer, Upgrade, TE). Keeping this here allows future optimization of storage layout without
  // scattering the logic.
  // You can use 'static_assert' to make sure at compilation time that the header you are about to insert is not
  // reserved. The list of reserved headers is unlikely to change in the future, but they are mostly technical /
  // framework headers that aeronet manages internally and probably not very interesting for the client.
  // Example:
  //     static_assert(!aeronet::HttpResponse::IsReservedHeader("X-My-Header")); // OK
  //     static_assert(!aeronet::HttpResponse::IsReservedHeader("Content-Length")); // Not OK
  [[nodiscard]] static constexpr bool IsReservedHeader(std::string_view name) noexcept {
    using namespace http;
    return name == Connection || name == Date || name == ContentLength || name == TransferEncoding || name == Trailer ||
           name == Upgrade || name == TE;
  }

 private:
  friend class HttpServer;
  friend class HttpResponseTest;
  friend class HttpResponseWriter;  // streaming writer needs access to finalize

  [[nodiscard]] std::size_t reasonLen() const noexcept {
    if (_data[kReasonBeg] == '\n') {
      return 0UL;
    }
    if (_headersStartPos != 0) {
      return _headersStartPos - http::CRLF.size() - kReasonBeg;
    }
    return _bodyStartPos - kReasonBeg - http::DoubleCRLF.size();
  }

  [[nodiscard]] std::size_t bodyLen() const noexcept { return _data.size() - _bodyStartPos; }

  void setReason(std::string_view newReason);

  void setBody(std::string_view newBody);

  void setHeader(std::string_view key, std::string_view value);

  void appendHeaderUnchecked(std::string_view key, std::string_view value);

  // IMPORTANT: This method finalizes the response by appending reserved headers.
  // After it returns, calling any mutating method (statusCode, reason, body,
  // appendHeader, header) is undefined behavior and may corrupt the buffer or
  // duplicate reserved headers. Higher-level server code is expected to call
  // this exactly once right before write.
  std::string_view finalizeAndGetFullTextResponse(std::string_view version, std::string_view date, bool keepAlive,
                                                  bool isHeadMethod);

  RawChars _data;
  uint32_t _headersStartPos{};  // position just after the CRLF that starts the first header line
  uint32_t _bodyStartPos{};     // position of first body byte (after CRLF CRLF)
};

}  // namespace aeronet