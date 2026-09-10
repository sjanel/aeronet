#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-event-sink.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/sv-to-sv-map.hpp"

namespace aeronet::http2 {

/// Test-only EventSink: each event forwards to an optional std::function, defaulting to no-op. Binds itself to the
/// connection on construction and detaches on destruction, so lifetime bugs (sink outliving/outlived-by the connection)
/// fail loudly instead of  dangling. Field names use the "Fn" suffix to avoid clashing with the virtual methods they
/// implement (onStreamClosed the method vs. onStreamClosedFn the assignable hook).
class RecordingEventSink final : public EventSink {
 public:
  explicit RecordingEventSink(http2::Http2Connection& conn) noexcept : _conn(conn) { _conn.setEventSink(this); }

  RecordingEventSink(const RecordingEventSink&) = delete;
  RecordingEventSink(RecordingEventSink&&) noexcept = delete;
  RecordingEventSink& operator=(const RecordingEventSink&) = delete;
  RecordingEventSink& operator=(RecordingEventSink&&) noexcept = delete;

  ~RecordingEventSink() override { _conn.setEventSink(nullptr); }

  void onHeadersDecoded(uint32_t sid, const SvToSvMap& hm, bool es) override {
    if (onHeadersDecodedFn) {
      onHeadersDecodedFn(sid, hm, es);
    }
  }
  void onData(uint32_t sid, std::span<const std::byte> data, bool es) override {
    if (onDataFn) {
      onDataFn(sid, data, es);
    }
  }
  void onStreamReset(uint32_t sid, http2::ErrorCode ec) override {
    if (onStreamResetFn) {
      onStreamResetFn(sid, ec);
    }
  }
  void onStreamClosed(uint32_t sid) override {
    if (onStreamClosedFn) {
      onStreamClosedFn(sid);
    }
  }
  void onGoAway(uint32_t last, http2::ErrorCode ec, std::string_view dbg) override {
    if (onGoAwayFn) {
      onGoAwayFn(last, ec, dbg);
    }
  }
  void onWindowUpdate(uint32_t sid, uint32_t inc) override {
    if (onWindowUpdateFn) {
      onWindowUpdateFn(sid, inc);
    }
  }

  std::function<void(uint32_t, const SvToSvMap&, bool)> onHeadersDecodedFn;
  std::function<void(uint32_t, std::span<const std::byte>, bool)> onDataFn;
  std::function<void(uint32_t, http2::ErrorCode)> onStreamResetFn;
  std::function<void(uint32_t)> onStreamClosedFn;
  std::function<void(uint32_t, http2::ErrorCode, std::string_view)> onGoAwayFn;
  std::function<void(uint32_t, uint32_t)> onWindowUpdateFn;

 private:
  http2::Http2Connection& _conn;
};

}  // namespace aeronet::http2