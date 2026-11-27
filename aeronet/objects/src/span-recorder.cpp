#include "span-recorder.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "aeronet/log.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/utf8-scalar.hpp"
#include "otlp-wire.hpp"

namespace aeronet::tracing::detail {

namespace {

// Size of the fields of a span written at its start, but its name.
constexpr std::size_t kSpanStartFieldsSize = otlp::VarintSize(otlp::kSpanTraceId) + 1 + otlp::kTraceIdSize +
                                             otlp::VarintSize(otlp::kSpanSpanId) + 1 + otlp::kSpanIdSize +
                                             otlp::VarintSize(otlp::kSpanName) + otlp::VarintSize(otlp::kSpanKind) + 1 +
                                             otlp::VarintSize(otlp::kSpanStartTimeUnixNano) + sizeof(uint64_t) +
                                             otlp::VarintSize(otlp::kSpanFlags) + sizeof(uint32_t);

constexpr std::size_t kSpanEndFieldsSize = otlp::VarintSize(otlp::kSpanEndTimeUnixNano) + sizeof(uint64_t);

// Capacity of a new record: a request span (5 attributes) fits, its record then never grows.
constexpr std::size_t kInitialRecordCapacity = 256;

int64_t SteadyNs(std::chrono::steady_clock::time_point timePoint) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(timePoint.time_since_epoch()).count();
}

std::size_t LengthDelimitedSize(uint32_t tag, std::size_t length) noexcept {
  return otlp::VarintSize(tag) + otlp::VarintSize(length) + length;
}

char* AppendLengthDelimited(uint32_t tag, std::string_view bytes, char* pDest) noexcept {
  pDest = otlp::AppendVarint(tag, pDest);
  pDest = otlp::AppendVarint(bytes.size(), pDest);
  return Append(bytes, pDest);
}

}  // namespace

SpanBatchHeader::SpanBatchHeader(std::string_view resource, std::string_view resourceSchemaUrl, std::string_view scope,
                                 std::string_view scopeSchemaUrl) {
  // ResourceSpans: resource, schema_url, then the scope_spans field of the spans. Its ScopeSpans: scope, schema_url,
  // then the spans. A parser accepts the fields in any order.
  const std::size_t resourceSpansHeadSize =
      LengthDelimitedSize(otlp::kResourceSpansResource, resource.size()) +
      (resourceSchemaUrl.empty() ? 0 : LengthDelimitedSize(otlp::kResourceSpansSchemaUrl, resourceSchemaUrl.size())) +
      otlp::VarintSize(otlp::kResourceSpansScopeSpans);
  const std::size_t scopeSpansHeadSize =
      LengthDelimitedSize(otlp::kScopeSpansScope, scope.size()) +
      (scopeSchemaUrl.empty() ? 0 : LengthDelimitedSize(otlp::kScopeSpansSchemaUrl, scopeSchemaUrl.size()));
  _scopeSpansLengthPos = resourceSpansHeadSize;
  _bytes.reserve(resourceSpansHeadSize + otlp::kMaxVarint32Size + scopeSpansHeadSize);
  char* pos = AppendLengthDelimited(otlp::kResourceSpansResource, resource, _bytes.data());
  if (!resourceSchemaUrl.empty()) {
    pos = AppendLengthDelimited(otlp::kResourceSpansSchemaUrl, resourceSchemaUrl, pos);
  }
  pos = otlp::AppendVarint(otlp::kResourceSpansScopeSpans, pos);
  // The length of the ScopeSpans, written by writeTo().
  pos = otlp::AppendPaddedVarint32(0, pos);
  pos = AppendLengthDelimited(otlp::kScopeSpansScope, scope, pos);
  if (!scopeSchemaUrl.empty()) {
    pos = AppendLengthDelimited(otlp::kScopeSpansSchemaUrl, scopeSchemaUrl, pos);
  }
  _bytes.setEnd(pos);
}

RawChars32 SpanBatchHeader::emptyBatch() const {
  // Its bytes are never read before writeTo() writes them.
  RawChars32 batch(_bytes.size());
  batch.setSize(_bytes.size());
  return batch;
}

void SpanBatchHeader::writeTo(RawChars32& batch) const noexcept {
  assert(batch.size() >= _bytes.size());
  char* data = batch.data();
  Copy(_bytes.data(), _bytes.size(), data);
  // The ScopeSpans field holds all the bytes after its length. A batch stays far below 4 GiB: at most kMaxQueuedSpans
  // spans of at most 8 attributes of request heads.
  const std::size_t scopeSpansPos = _scopeSpansLengthPos + otlp::kMaxVarint32Size;
  assert(batch.size() - scopeSpansPos <= UINT32_MAX);
  otlp::AppendPaddedVarint32(static_cast<uint32_t>(batch.size() - scopeSpansPos), data + _scopeSpansLengthPos);
}

void SpanRecord::start(std::string_view name, uint64_t traceIdHigh, uint64_t traceIdLow, uint64_t spanId,
                       uint64_t startUnixNs, std::chrono::steady_clock::time_point startTime) noexcept {
  _startTime = startTime;
  _startUnixNs = startUnixNs;
  _ended = false;
  _fields.clear();
  _fields.ensureAvailableCapacity(
      std::max(kSpanStartFieldsSize + otlp::VarintSize(name.size()) + name.size(), kInitialRecordCapacity));
  char* pos = otlp::AppendVarint(otlp::kSpanTraceId, _fields.data());
  *pos++ = static_cast<char>(otlp::kTraceIdSize);
  pos = otlp::AppendLittleEndian(traceIdHigh, pos);
  pos = otlp::AppendLittleEndian(traceIdLow, pos);
  pos = otlp::AppendVarint(otlp::kSpanSpanId, pos);
  *pos++ = static_cast<char>(otlp::kSpanIdSize);
  pos = otlp::AppendLittleEndian(spanId, pos);
  pos = AppendLengthDelimited(otlp::kSpanName, name, pos);
  pos = otlp::AppendVarint(otlp::kSpanKind, pos);
  *pos++ = static_cast<char>(otlp::kSpanKindInternal);
  pos = otlp::AppendVarint(otlp::kSpanStartTimeUnixNano, pos);
  pos = otlp::AppendLittleEndian(startUnixNs, pos);
  pos = otlp::AppendVarint(otlp::kSpanFlags, pos);
  pos = otlp::AppendLittleEndian(otlp::kSpanFlagsSampled, pos);
  _fields.setEnd(pos);
}

template <class WriteValue>
void SpanRecord::appendAttribute(std::string_view key, std::size_t anyValueSize, WriteValue writeValue) noexcept {
  const std::size_t keyValueSize =
      LengthDelimitedSize(otlp::kKeyValueKey, key.size()) + LengthDelimitedSize(otlp::kKeyValueValue, anyValueSize);
  _fields.ensureAvailableCapacityExponential(LengthDelimitedSize(otlp::kSpanAttributes, keyValueSize));
  char* pos = otlp::AppendVarint(otlp::kSpanAttributes, _fields.end());
  pos = otlp::AppendVarint(keyValueSize, pos);
  pos = AppendLengthDelimited(otlp::kKeyValueKey, key, pos);
  pos = otlp::AppendVarint(otlp::kKeyValueValue, pos);
  pos = otlp::AppendVarint(anyValueSize, pos);
  _fields.setEnd(writeValue(pos));
}

void SpanRecord::setAttribute(std::string_view key, int64_t val) noexcept {
  // The SDK ignores the attributes of an empty key, and those set once the span ended.
  if (key.empty() || _ended) {
    return;
  }
  // An int64 is the varint of its two's complement (10 bytes when negative).
  const auto value = static_cast<uint64_t>(val);
  appendAttribute(key, otlp::VarintSize(otlp::kAnyValueIntValue) + otlp::VarintSize(value), [value](char* pos) {
    pos = otlp::AppendVarint(otlp::kAnyValueIntValue, pos);
    return otlp::AppendVarint(value, pos);
  });
}

void SpanRecord::setAttribute(std::string_view key, std::string_view val) noexcept {
  if (key.empty() || _ended) {
    return;
  }
  const auto* pFirst = reinterpret_cast<const uint8_t*>(val.data());
  const uint32_t valueTag =
      IsValidUtf8Scalar(pFirst, pFirst + val.size()) ? otlp::kAnyValueStringValue : otlp::kAnyValueBytesValue;
  appendAttribute(key, LengthDelimitedSize(valueTag, val.size()),
                  [valueTag, val](char* pos) { return AppendLengthDelimited(valueTag, val, pos); });
}

void SpanRecord::end() noexcept {
  if (_ended) {
    return;
  }
  _ended = true;
  const auto duration = std::chrono::steady_clock::now() - _startTime;
  const uint64_t endUnixNs =
      _startUnixNs + static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
  _fields.ensureAvailableCapacityExponential(kSpanEndFieldsSize);
  char* pos = otlp::AppendVarint(otlp::kSpanEndTimeUnixNano, _fields.end());
  _fields.setEnd(otlp::AppendLittleEndian(endUnixNs, pos));
  _pRecorder->endSpan(*this);
}

void SpanRecord::recycle() noexcept { _pRecorder->recycle(*this); }

SpanBatchExporter::SpanBatchExporter(std::unique_ptr<SpanBatchSink> sink, SpanBatchHeader header)
    : _sink(std::move(sink)),
      _header(std::move(header)),
      _waiting(_header.emptyBatch()),
      _sending(_header.emptyBatch()),
      _thread([this] { run(); }) {}

SpanBatchExporter::~SpanBatchExporter() {
  if (_thread.joinable()) {
    RawChars32 noBatch;
    finish(noBatch, 0);
  }
}

bool SpanBatchExporter::tryHandoff(RawChars32& batch, uint32_t nbSpans) noexcept {
  // Only the thread of the recorder makes a batch wait: once the thread took it, the slot stays free until then.
  if (_hasWaitingBatch.load(std::memory_order_relaxed)) {
    return false;
  }
  {
    const std::scoped_lock lock(_mutex);
    assert(_waiting.size() == _header.size());
    _waiting.swap(batch);
    _nbWaitingSpans = nbSpans;
    _hasWaitingBatch.store(true, std::memory_order_relaxed);
  }
  _batchWaiting.notify_one();
  return true;
}

void SpanBatchExporter::finish(RawChars32& lastBatch, uint32_t nbSpans) noexcept {
  {
    std::unique_lock<std::mutex> lock(_mutex);
    _waitingTaken.wait(lock, [this] { return !_hasWaitingBatch.load(std::memory_order_relaxed); });
    if (nbSpans != 0) {
      _waiting.swap(lastBatch);
      _nbWaitingSpans = nbSpans;
      _hasWaitingBatch.store(true, std::memory_order_relaxed);
    }
    _stopRequested = true;
  }
  _batchWaiting.notify_one();
  _thread.join();
  _sink->shutdown();
}

void SpanBatchExporter::run() noexcept {
  std::unique_lock<std::mutex> lock(_mutex);
  while (true) {
    _batchWaiting.wait(lock, [this] { return _hasWaitingBatch.load(std::memory_order_relaxed) || _stopRequested; });
    if (!_hasWaitingBatch.load(std::memory_order_relaxed)) {
      // Stopped, every batch sent.
      return;
    }
    _sending.swap(_waiting);
    _nbSendingSpans = _nbWaitingSpans;
    _hasWaitingBatch.store(false, std::memory_order_relaxed);
    lock.unlock();
    _waitingTaken.notify_one();

    _header.writeTo(_sending);
    _sink->send(_sending, _nbSendingSpans);
    _sending.setSize(static_cast<RawChars32::size_type>(_header.size()));

    lock.lock();
  }
}

SpanRecorder::SpanRecorder(std::unique_ptr<SpanBatchSink> sink, SpanBatchHeader header,
                           std::chrono::milliseconds exportInterval, uint64_t seed)
    : _batch(header.emptyBatch()),
      _ids(seed),
      _exportInterval(exportInterval),
      _exporter(std::move(sink), std::move(header)) {
  resetClock();
}

SpanRecorder::~SpanRecorder() {
  // The contract of TelemetryContext::createSpan().
  assert(allSpansReleased());
  _exporter.finish(_batch, _nbBatchSpans);
  logDroppedSpans();
}

bool SpanRecorder::allSpansReleased() const noexcept {
  uint32_t nbFreeRecords = 0;
  for (const SpanRecord* pRecord = _pFreeRecords; pRecord != nullptr; pRecord = pRecord->_pNextFree) {
    ++nbFreeRecords;
  }
  return nbFreeRecords == _records.size();
}

SpanPtr SpanRecorder::createSpan(std::string_view name) noexcept {
  SpanRecord* pRecord = _pFreeRecords;
  if (pRecord != nullptr) {
    _pFreeRecords = pRecord->_pNextFree;
  } else {
    pRecord = _records.allocateAndConstruct(*this);
  }

  // Random ids from the generator of the recorder, as the random id generator of the SDK makes. An all zero id is
  // invalid: SplitMix64 draws 0 once in 2^64 values.
  const uint64_t traceIdHigh = _ids.next();
  uint64_t traceIdLow = _ids.next();
  traceIdLow |= static_cast<uint64_t>((traceIdHigh | traceIdLow) == 0);
  uint64_t spanId = _ids.next();
  spanId |= static_cast<uint64_t>(spanId == 0);

  const auto now = std::chrono::steady_clock::now();
  pRecord->start(name, traceIdHigh, traceIdLow, spanId, static_cast<uint64_t>(SteadyNs(now) + _unixMinusSteadyNs), now);
  return SpanPtr(pRecord);
}

void SpanRecorder::endSpan(const SpanRecord& record) noexcept {
  if (_nbBatchSpans == kMaxQueuedSpans && !tryHandoff()) {
    ++_nbDroppedSpans;
    return;
  }
  const std::string_view fields = record._fields;
  _batch.ensureAvailableCapacityExponential(LengthDelimitedSize(otlp::kScopeSpansSpans, fields.size()));
  _batch.setEnd(AppendLengthDelimited(otlp::kScopeSpansSpans, fields, _batch.end()));
  if (++_nbBatchSpans >= kSpansPerBatch) {
    tryHandoff();
  }
}

void SpanRecorder::recycle(SpanRecord& record) noexcept {
  // The SDK exports a span destroyed without End() as well.
  record.end();
  record._pNextFree = _pFreeRecords;
  _pFreeRecords = &record;
}

bool SpanRecorder::tryHandoff() noexcept {
  if (!_exporter.tryHandoff(_batch, _nbBatchSpans)) {
    return false;
  }
  _nbBatchSpans = 0;
  logDroppedSpans();
  resetClock();
  return true;
}

void SpanRecorder::logDroppedSpans() noexcept {
  if (_nbDroppedSpans != 0) {
    log::warn("{} spans dropped: the trace exporter could not keep up with {} queued spans", _nbDroppedSpans,
              kMaxQueuedSpans);
    _nbDroppedSpans = 0;
  }
}

void SpanRecorder::flush() noexcept {
  if (_nbBatchSpans != 0 && std::chrono::steady_clock::now() - _lastHandoff >= _exportInterval) {
    tryHandoff();
  }
}

void SpanRecorder::resetClock() noexcept {
  const auto systemNow = std::chrono::system_clock::now();
  _lastHandoff = std::chrono::steady_clock::now();
  _unixMinusSteadyNs = std::chrono::duration_cast<std::chrono::nanoseconds>(systemNow.time_since_epoch()).count() -
                       SteadyNs(_lastHandoff);
}

}  // namespace aeronet::tracing::detail
