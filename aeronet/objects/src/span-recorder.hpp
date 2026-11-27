#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

#include "aeronet/chrono-lite.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "split-mix64.hpp"

// The spans of a TelemetryContext, recorded by aeronet itself rather than by the trace SDK of opentelemetry-cpp: each
// span is encoded as an OTLP protobuf Span message as it is made, in a record of its recorder reused from span to span,
// then appended to the batch of the recorder when it ends. A thread of the recorder sends the batches. Once warm,
// making a span allocates nothing, and sending a batch allocates only in the sink (the HTTP request). The recorder
// outlives its spans: they are all released before it is destroyed.
namespace aeronet::tracing::detail {

class SpanRecorder;

// Sends the batches of spans of a SpanBatchExporter, from its thread.
class SpanBatchSink {
 public:
  SpanBatchSink() noexcept = default;

  SpanBatchSink(const SpanBatchSink&) = delete;
  SpanBatchSink(SpanBatchSink&&) = delete;
  SpanBatchSink& operator=(const SpanBatchSink&) = delete;
  SpanBatchSink& operator=(SpanBatchSink&&) = delete;

  virtual ~SpanBatchSink() = default;

  // Sends a batch of 'nbSpans' spans: 'resourceSpans' holds the encoded fields of an OTLP ResourceSpans message, its
  // resource and the ScopeSpans with the spans, valid until the call returns.
  virtual void send(std::string_view resourceSpans, uint32_t nbSpans) noexcept = 0;

  // Called once, after the last send().
  virtual void shutdown() noexcept = 0;
};

// The start of the bytes of every batch: the encoded fields of the ResourceSpans message before its spans, ending with
// the ScopeSpans field holding the spans. The length of the latter, unknown until the batch is sent, is written then
// in a varint of a fixed size (see otlp::AppendPaddedVarint32()).
class SpanBatchHeader {
 public:
  // The encoded Resource and InstrumentationScope messages, and their schema URLs (not encoded when empty).
  SpanBatchHeader(std::string_view resource, std::string_view resourceSchemaUrl, std::string_view scope,
                  std::string_view scopeSchemaUrl);

  [[nodiscard]] std::size_t size() const noexcept { return _bytes.size(); }

  // A batch without span: room for the header, written by writeTo() when the batch is sent.
  [[nodiscard]] RawChars32 emptyBatch() const;

  // Writes the header at the start of 'batch', a header followed by the encoded spans.
  void writeTo(RawChars32& batch) const noexcept;

 private:
  RawChars32 _bytes;
  // Position in _bytes of the varint of the length of the ScopeSpans field.
  std::size_t _scopeSpansLengthPos;
};

// A span of a SpanRecorder: the fields of its OTLP Span message, encoded as they are set.
class SpanRecord final : public Span {
 public:
  // A record of 'recorder', made by its pool.
  explicit SpanRecord(SpanRecorder& recorder) noexcept : _pRecorder(&recorder) {}

  void setAttribute(std::string_view key, int64_t val) noexcept override;

  // An invalid UTF-8 value is encoded as bytes, as the OTLP exporter of opentelemetry-cpp does: a string field holding
  // invalid UTF-8 would make a collector reject the whole batch.
  void setAttribute(std::string_view key, std::string_view val) noexcept override;

  void end() noexcept override;

  void recycle() noexcept override;

 private:
  friend class SpanRecorder;

  // Starts a new span in this record, created at 'startTime' (steady clock), 'startUnixNs' in system time.
  void start(std::string_view name, uint64_t traceIdHigh, uint64_t traceIdLow, uint64_t spanId, uint64_t startUnixNs,
             std::chrono::steady_clock::time_point startTime) noexcept;

  // Appends an attribute to the span: a KeyValue whose AnyValue is 'anyValueSize' bytes long, written by writeValue.
  template <class WriteValue>
  void appendAttribute(std::string_view key, std::size_t anyValueSize, WriteValue writeValue) noexcept;

  RawChars32 _fields;
  SpanRecorder* _pRecorder;
  // Next record of the free list of the recorder, while this one is in it.
  SpanRecord* _pNextFree{nullptr};
  std::chrono::steady_clock::time_point _startTime;
  uint64_t _startUnixNs{0};
  bool _ended{false};
};

// The thread exporting the batches of a SpanRecorder, which hands each batch off to it in a buffer swap: one lock and
// one wakeup per batch. Three buffers rotate: the batch filled by the recorder, the one waiting for the thread, the one
// the thread sends. Their capacity is kept from batch to batch.
class SpanBatchExporter {
 public:
  SpanBatchExporter(std::unique_ptr<SpanBatchSink> sink, SpanBatchHeader header);

  SpanBatchExporter(const SpanBatchExporter&) = delete;
  SpanBatchExporter(SpanBatchExporter&&) = delete;
  SpanBatchExporter& operator=(const SpanBatchExporter&) = delete;
  SpanBatchExporter& operator=(SpanBatchExporter&&) = delete;

  // Stops the thread (see finish()) if not done yet.
  ~SpanBatchExporter();

  // Hands 'batch', of 'nbSpans' spans, off to the thread, which sends it, and gives back an empty batch in it. Returns
  // false, 'batch' left as is, while the previous batch is still waiting for the thread.
  [[nodiscard]] bool tryHandoff(RawChars32& batch, uint32_t nbSpans) noexcept;

  // Hands 'lastBatch' off (if it has spans) once the thread took the previous one, waits until the thread sent them
  // all, stops it and shuts the sink down.
  void finish(RawChars32& lastBatch, uint32_t nbSpans) noexcept;

 private:
  void run() noexcept;

  std::unique_ptr<SpanBatchSink> _sink;
  SpanBatchHeader _header;
  // The batch waiting for the thread when _hasWaitingBatch, else an empty batch.
  RawChars32 _waiting;
  // The batch sent by the thread, or an empty one.
  RawChars32 _sending;
  std::mutex _mutex;
  std::condition_variable _batchWaiting;
  std::condition_variable _waitingTaken;
  uint32_t _nbWaitingSpans{0};
  uint32_t _nbSendingSpans{0};
  // Written under _mutex, read without it by tryHandoff(): a batch still waiting costs no lock.
  std::atomic<bool> _hasWaitingBatch{false};
  bool _stopRequested{false};
  // Last: started once the members it uses are constructed.
  std::thread _thread;
};

// The spans of a TelemetryContext, all used on the thread of the context: a pool of records, the batch of the ended
// spans, the ids and clock of the spans, and the exporter thread.
// Its spans are all released before it is destroyed (see TelemetryContext::createSpan()): ServerContext destroys its
// connections, and their requests with their span, before its telemetry.
class SpanRecorder {
 public:
  // Spans per batch: a batch is handed off to the exporter as soon as it has that many spans (the default
  // max_export_batch_size of the batch span processor of the SDK).
  static constexpr uint32_t kSpansPerBatch = 512;

  // Spans of a batch while the exporter is busy with the previous ones (the default max_queue_size of the batch span
  // processor): the next ones are dropped until a handoff.
  static constexpr uint32_t kMaxQueuedSpans = 2048;

  // Records the spans, sent to 'sink' by the exporter thread in batches starting with 'header', of kSpansPerBatch
  // spans, or of the spans ended at a flush() once 'exportInterval' has passed since the last batch. Their ids are
  // drawn from 'seed'.
  SpanRecorder(std::unique_ptr<SpanBatchSink> sink, SpanBatchHeader header, std::chrono::milliseconds exportInterval,
               uint64_t seed);

  SpanRecorder(const SpanRecorder&) = delete;
  SpanRecorder(SpanRecorder&&) = delete;
  SpanRecorder& operator=(const SpanRecorder&) = delete;
  SpanRecorder& operator=(SpanRecorder&&) = delete;

  // Exports the ended spans and stops the exporter thread. Every span is released already.
  ~SpanRecorder();

  // Starts a span: a sampled root span of a new trace, of kind internal.
  [[nodiscard]] SpanPtr createSpan(std::string_view name) noexcept;

  // Hands the ended spans off to the exporter if exportInterval has passed since the last batch.
  void flush() noexcept;

 private:
  friend class SpanRecord;

  // Appends the span of 'record', just ended, to the batch.
  void endSpan(const SpanRecord& record) noexcept;

  // Returns 'record', ended first if it was not, to the free list.
  void recycle(SpanRecord& record) noexcept;

  // Whether every record of the pool is in the free list: no span is outstanding (checked at destruction).
  [[nodiscard]] bool allSpansReleased() const noexcept;

  bool tryHandoff() noexcept;

  // Logs the spans dropped since the last call, if any.
  void logDroppedSpans() noexcept;

  // Refreshes the offset of the system clock from the steady clock, and the time of the last handoff.
  void resetClock() noexcept;

  // Before _exporter: made from the header before the exporter takes it.
  RawChars32 _batch;
  // The records of the pool not used by a span, linked by their _pNextFree.
  SpanRecord* _pFreeRecords{nullptr};
  SplitMix64 _ids;
  std::chrono::steady_clock::duration _exportInterval;
  std::chrono::steady_clock::time_point _lastHandoff;
  // System clock minus steady clock, in nanoseconds: the system time of a steady time point.
  int64_t _unixMinusSteadyNs{0};
  uint32_t _nbBatchSpans{0};
  uint32_t _nbDroppedSpans{0};
  // All the records, made when the free list is empty and never released: a record keeps the capacity of its fields
  // from span to span. Destroyed with the recorder.
  ObjectPool<SpanRecord> _records;
  // Last: its thread starts once the recorder is constructed.
  SpanBatchExporter _exporter;
};

}  // namespace aeronet::tracing::detail
