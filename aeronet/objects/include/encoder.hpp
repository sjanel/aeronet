#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

// =============================================================================
// aeronet Encoding Abstraction
// =============================================================================
// Rationale (Updated Streaming Design)
// ------------------------------------
// We split responsibilities between two interfaces:
//   * Encoder: stateless (or configuration-only) object providing one-shot compression.
//   * EncoderContext: stateful streaming object created from an Encoder via makeContext().
// This mirrors the C++17 node extraction pattern (producer creates a movable, owning state
// object) and removes the need to embed mutable codec state (z_stream, dictionaries, etc.)
// inside the Encoder itself. It enables:
//   - One static / per-thread encoder prototype.
//   - Multiple independent streaming contexts (future multiplexing) without cloning encoders.
//   - Clear lifecycle: context -> encode()* -> finish(last encode with finish=true) -> destroy.
//   - Potential pooling / reuse of contexts without touching the encoder’s own type.
//
// Contracts Summary
// -----------------
// Encoder (one-shot):
//   encode(data): compress the entire buffer; returns view valid until next encode() on same encoder.
// EncoderContext (streaming):
//   encode(data, finish): incremental compression; finish=true flushes & finalizes. After finish=true
//   only encode({}, false) is allowed and must return empty (or implementation may assert in debug builds).
//
// Thread Safety: Both Encoder and created EncoderContext instances are not thread-safe. Each context
// must be confined to a single response lifecycle within a single thread.
//
// Error Handling: Implementations may throw on initialization or fatal internal codec errors. Identity
// variants never throw.
//
// Extension Points: Adding a new codec only requires providing a subclass of Encoder + a matching
// EncoderContext implementation.
//
// Minimal Example (Streaming):
//   auto ctx = encoder.makeContext();
//   for(auto chunk : chunks){ auto out = ctx->encode(chunk,false); if(!out.empty()) queue(out); }
//   auto last = ctx->encode({}, true); if(!last.empty()) queue(last);
//
// Minimal Example (One-shot):
//   auto out = encoder.encode(fullBody);
//   queue(out);
//
// =============================================================================

namespace aeronet {

class EncoderContext {
 public:
  virtual ~EncoderContext() = default;

  virtual std::string_view encodeChunk(std::size_t encoderChunkSize, std::string_view data, bool finish) = 0;
};

class Encoder {
 public:
  virtual ~Encoder() = default;
  // One-shot full-buffer compression (no streaming state). Implementations may reuse an internal buffer.
  virtual std::string_view encodeFull(std::size_t encoderChunkSize, std::string_view full) = 0;

  // Create a streaming context. Each context is independent.
  virtual std::unique_ptr<EncoderContext> makeContext() = 0;
};

// Identity / pass‑through encoder. Does not own a buffer and returns input
// directly (except when finish && data.empty() -> returns empty view).
class IdentityEncoderContext : public EncoderContext {
 public:
  std::string_view encodeChunk([[maybe_unused]] std::size_t encoderChunkSize, std::string_view data,
                               [[maybe_unused]] bool finish) override {
    return data;
  }
};

class IdentityEncoder : public Encoder {
 public:
  std::string_view encodeFull([[maybe_unused]] std::size_t encoderChunkSize, std::string_view full) override {
    return full;
  }

  std::unique_ptr<EncoderContext> makeContext() override { return std::make_unique<IdentityEncoderContext>(); }
};

}  // namespace aeronet