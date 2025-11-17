#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/raw-chars.hpp"

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
// Error Handling: Implementations may throw on initialization or fatal internal codec errors.
//
// Extension Points: Adding a new codec only requires providing a subclass of Encoder + a matching
// EncoderContext implementation.
// =============================================================================

namespace aeronet {

class EncoderContext {
 public:
  virtual ~EncoderContext() = default;

  // Streaming chunk encoder. If 'data' is empty, it will be considered as a finish.
  virtual std::string_view encodeChunk(std::size_t encoderChunkSize, std::string_view data) = 0;
};

class Encoder {
 public:
  virtual ~Encoder() = default;

  // One-shot full-buffer compression (no streaming state). Implementations may reuse an internal buffer.
  // The encoder will append compressed data to the provided 'buf' RawChars instance.
  // 'extraCapacity': additional capacity to ensure in 'buf' before encoding (to avoid multiple reallocations).
  virtual void encodeFull(std::size_t extraCapacity, std::string_view data, RawChars &buf) = 0;

  // Create a streaming context. Each context is independent.
  virtual std::unique_ptr<EncoderContext> makeContext() = 0;
};

}  // namespace aeronet