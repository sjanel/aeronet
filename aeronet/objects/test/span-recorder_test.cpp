#include "../src/span-recorder.hpp"

#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>
#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/common/timestamp.h>
#include <opentelemetry/exporters/otlp/otlp_recordable.h>
#include <opentelemetry/exporters/otlp/otlp_recordable_utils.h>
#include <opentelemetry/nostd/span.h>
#include <opentelemetry/nostd/string_view.h>
#include <opentelemetry/proto/collector/trace/v1/trace_service.pb.h>
#include <opentelemetry/proto/trace/v1/trace.pb.h>
#include <opentelemetry/sdk/instrumentationscope/instrumentation_scope.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/recordable.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/trace/span_id.h>
#include <opentelemetry/trace/span_metadata.h>
#include <opentelemetry/trace/trace_flags.h>
#include <opentelemetry/trace/trace_id.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../src/otlp-span-export.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tracing/tracer.hpp"

namespace aeronet::tracing::detail {
namespace {

using namespace std::chrono_literals;

using opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest;
using OtlpSpan = opentelemetry::proto::trace::v1::Span;

// The batches of a TestSink, kept by the test: the sink belongs to the exporter.
struct SinkState {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::string> batches;
  // The number of spans of each batch, as told by the exporter.
  std::vector<uint32_t> batchSpans;
  int nbSendsStarted{0};
  // While true, send() blocks: a collector that does not answer.
  bool stalled{false};
  bool shutdown{false};
};

class TestSink final : public SpanBatchSink {
 public:
  explicit TestSink(SinkState& state) noexcept : _state(state) {}

  void send(std::string_view resourceSpans, uint32_t nbSpans) noexcept override {
    std::unique_lock<std::mutex> lock(_state.mutex);
    ++_state.nbSendsStarted;
    _state.cv.notify_all();
    _state.cv.wait(lock, [this] { return !_state.stalled; });
    _state.batches.emplace_back(resourceSpans);
    _state.batchSpans.push_back(nbSpans);
    _state.cv.notify_all();
  }

  void shutdown() noexcept override {
    const std::scoped_lock lock(_state.mutex);
    _state.shutdown = true;
  }

 private:
  SinkState& _state;
};

const opentelemetry::sdk::resource::Resource& TestResource() {
  static const auto kResource =
      opentelemetry::sdk::resource::Resource::Create({{"service.name", "span-recorder-test"}});
  return kResource;
}

std::unique_ptr<SpanRecorder> MakeRecorder(SinkState& state,
                                           std::chrono::milliseconds exportInterval = std::chrono::hours{1}) {
  return std::make_unique<SpanRecorder>(std::make_unique<TestSink>(state), MakeOtlpSpanBatchHeader(TestResource()),
                                        exportInterval, 42);
}

// Waits until the sink received nbBatches batches, or 5 seconds.
bool WaitForBatches(SinkState& state, std::size_t nbBatches) {
  std::unique_lock<std::mutex> lock(state.mutex);
  return state.cv.wait_for(lock, 5s, [&] { return state.batches.size() >= nbBatches; });
}

bool WaitForSendsStarted(SinkState& state, int nbSends) {
  std::unique_lock<std::mutex> lock(state.mutex);
  return state.cv.wait_for(lock, 5s, [&] { return state.nbSendsStarted >= nbSends; });
}

// A batch as a request: it is the encoded ResourceSpans of the request.
ExportTraceServiceRequest ParseBatch(std::string_view batch) {
  ExportTraceServiceRequest request;
  EXPECT_TRUE(request.add_resource_spans()->ParseFromArray(batch.data(), static_cast<int>(batch.size())));
  return request;
}

const auto& SpansOf(const ExportTraceServiceRequest& request) {
  return request.resource_spans(0).scope_spans(0).spans();
}

struct TestAttribute {
  std::string key;
  std::variant<int64_t, std::string> value;
};

struct TestSpan {
  std::string name;
  std::vector<TestAttribute> attributes;
};

void Record(SpanRecorder& recorder, const TestSpan& testSpan) {
  const SpanPtr span = recorder.createSpan(testSpan.name);
  for (const TestAttribute& attribute : testSpan.attributes) {
    std::visit([&](const auto& value) { span->setAttribute(attribute.key, value); }, attribute.value);
  }
  span->end();
}

// The request that the OTLP exporter of opentelemetry-cpp makes of spans recorded by its SDK as 'spans', in its
// OtlpRecordable, called as the Span of the SDK calls it, with the ids and times of the spans of 'actual'.
ExportTraceServiceRequest SdkRequest(const ExportTraceServiceRequest& actual, std::span<const TestSpan> spans) {
  namespace otel = opentelemetry;
  const auto scope = otel::sdk::instrumentationscope::InstrumentationScope::Create("aeronet", AERONET_VERSION_STR);
  std::vector<std::unique_ptr<otel::sdk::trace::Recordable>> recordables;
  const auto& actualSpans = SpansOf(actual);
  EXPECT_EQ(static_cast<std::size_t>(actualSpans.size()), spans.size());
  for (std::size_t pos = 0; pos < spans.size() && std::cmp_less(pos, actualSpans.size()); ++pos) {
    const OtlpSpan& actualSpan = actualSpans[static_cast<int>(pos)];
    const TestSpan& testSpan = spans[pos];
    EXPECT_EQ(actualSpan.trace_id().size(), otel::trace::TraceId::kSize);
    EXPECT_EQ(actualSpan.span_id().size(), otel::trace::SpanId::kSize);
    const otel::trace::TraceId traceId(otel::nostd::span<const uint8_t, otel::trace::TraceId::kSize>(
        reinterpret_cast<const uint8_t*>(actualSpan.trace_id().data()), otel::trace::TraceId::kSize));
    const otel::trace::SpanId spanId(otel::nostd::span<const uint8_t, otel::trace::SpanId::kSize>(
        reinterpret_cast<const uint8_t*>(actualSpan.span_id().data()), otel::trace::SpanId::kSize));
    const otel::trace::SpanContext context(traceId, spanId,
                                           otel::trace::TraceFlags(otel::trace::TraceFlags::kIsSampled), false);

    auto recordable = std::make_unique<otel::exporter::otlp::OtlpRecordable>();
    recordable->SetName(testSpan.name);
    recordable->SetInstrumentationScope(*scope);
    recordable->SetIdentity(context, otel::trace::SpanId());
    recordable->SetTraceFlags(context.trace_flags());
    for (const TestAttribute& attribute : testSpan.attributes) {
      if (attribute.key.empty()) {
        // Skipped by OtlpRecordable from opentelemetry-cpp 1.29 on, by SpanRecord always.
        continue;
      }
      if (const auto* pInt = std::get_if<int64_t>(&attribute.value)) {
        recordable->SetAttribute(attribute.key, otel::common::AttributeValue{*pInt});
      } else {
        recordable->SetAttribute(attribute.key, otel::common::AttributeValue{
                                                    otel::nostd::string_view(std::get<std::string>(attribute.value))});
      }
    }
    recordable->SetSpanKind(otel::trace::SpanKind::kInternal);
    recordable->SetStartTime(
        otel::common::SystemTimestamp(std::chrono::nanoseconds(actualSpan.start_time_unix_nano())));
    recordable->SetResource(TestResource());
    recordable->SetDuration(
        std::chrono::nanoseconds(actualSpan.end_time_unix_nano() - actualSpan.start_time_unix_nano()));
    recordables.push_back(std::move(recordable));
  }
  ExportTraceServiceRequest expected;
  otel::exporter::otlp::OtlpRecordableUtils::PopulateRequest(
      otel::nostd::span<std::unique_ptr<otel::sdk::trace::Recordable>>(recordables.data(), recordables.size()),
      &expected);
  return expected;
}

void ExpectEqualRequests(const ExportTraceServiceRequest& expected, const ExportTraceServiceRequest& actual) {
  google::protobuf::util::MessageDifferencer differencer;
  std::string differences;
  differencer.ReportDifferencesToString(&differences);
  EXPECT_TRUE(differencer.Compare(expected, actual)) << differences;
}

// Spans and attributes of all the encodings: empty values, lengths of 1, 2 and 3 varint bytes, negative and extreme
// integers, attributes beyond the initial capacity of a record, multibyte UTF-8, duplicate keys.
std::vector<TestSpan> OracleSpans() {
  std::vector<TestAttribute> requestAttributes{
      {"http.method", "GET"},
      {"http.target", "/p"},
      {"http.scheme", "http"},
      {"http.host", "localhost:8080"},
      {"http.status_code", int64_t{200}},
  };
  std::vector<TestAttribute> edgeAttributes{
      {"empty.value", ""},
      {"", "empty key"},
      {"zero", int64_t{0}},
      {"minus.one", int64_t{-1}},
      {"min", std::numeric_limits<int64_t>::min()},
      {"max", std::numeric_limits<int64_t>::max()},
      {"utf8", "h\xC3\xA9llo w\xC3\xB6rld \xE2\x9C\x93 \xF0\x9F\x98\x80"},
      {"dup", int64_t{1}},
      {"dup", "two"},
  };
  std::vector<TestSpan> spans;
  spans.reserve(5);
  spans.push_back({"http.request", std::move(requestAttributes)});
  spans.push_back({"", {}});
  spans.push_back({"aeronet.middleware", std::move(edgeAttributes)});
  std::vector<TestAttribute> longAttributes;
  longAttributes.reserve(8);
  for (const std::size_t length : {std::size_t{127}, std::size_t{128}, std::size_t{16383}, std::size_t{16384}}) {
    longAttributes.push_back({std::string(length, 'k'), "v"});
    longAttributes.push_back({"value." + std::to_string(length), std::string(length, 'v')});
  }
  spans.push_back({std::string(300, 'n'), std::move(longAttributes)});
  std::vector<TestAttribute> manyAttributes;
  manyAttributes.reserve(40);
  for (int64_t pos = 0; pos < 40; ++pos) {
    manyAttributes.push_back({"attribute." + std::to_string(pos), pos * 1000});
  }
  spans.push_back({"many.attributes", std::move(manyAttributes)});
  return spans;
}

TEST(SpanRecorder, EncodesSpansAsTheOtlpExporterOfTheSdk) {
  SinkState state;
  const std::vector<TestSpan> spans = OracleSpans();
  {
    const std::unique_ptr<SpanRecorder> recorder = MakeRecorder(state);
    for (const TestSpan& testSpan : spans) {
      Record(*recorder, testSpan);
    }
  }
  ASSERT_EQ(state.batches.size(), 1U);
  EXPECT_EQ(state.batchSpans[0], spans.size());
  const ExportTraceServiceRequest actual = ParseBatch(state.batches[0]);
  ExpectEqualRequests(SdkRequest(actual, spans), actual);
}

// The same spans again in reused records, whose previous span was longer.
TEST(SpanRecorder, ReusedRecordsEncodeTheirNewSpanOnly) {
  SinkState state;
  const std::vector<TestSpan> spans = OracleSpans();
  {
    const std::unique_ptr<SpanRecorder> recorder = MakeRecorder(state);
    for (std::size_t pos = spans.size(); pos != 0; --pos) {
      Record(*recorder, spans[pos - 1]);
    }
    for (const TestSpan& testSpan : spans) {
      Record(*recorder, testSpan);
    }
  }
  ASSERT_EQ(state.batches.size(), 1U);
  ExportTraceServiceRequest actual = ParseBatch(state.batches[0]);
  // The second half, in the order of 'spans'.
  auto* pSpans = actual.mutable_resource_spans(0)->mutable_scope_spans(0)->mutable_spans();
  pSpans->DeleteSubrange(0, static_cast<int>(spans.size()));
  ExpectEqualRequests(SdkRequest(actual, spans), actual);
}

TEST(SpanRecorder, ReusesItsRecords) {
  SinkState state;
  const std::unique_ptr<SpanRecorder> recorder = MakeRecorder(state);
  SpanPtr first = recorder->createSpan("first");
  SpanPtr second = recorder->createSpan("second");
  const Span* pFirst = first.get();
  const Span* pSecond = second.get();
  first.reset();
  second.reset();
  // Last in, first out.
  const SpanPtr third = recorder->createSpan("third");
  const SpanPtr fourth = recorder->createSpan("fourth");
  EXPECT_EQ(third.get(), pSecond);
  EXPECT_EQ(fourth.get(), pFirst);
}

// More spans at once than the first block of the pool of records holds: the records of the first block stay valid
// while the pool grows, and they are all reused afterwards.
TEST(SpanRecorder, ManySpansAtOnce) {
  static constexpr std::size_t kNbSpans = 100;
  SinkState state;
  {
    const std::unique_ptr<SpanRecorder> recorder = MakeRecorder(state);
    for (int round = 0; round < 2; ++round) {
      std::vector<SpanPtr> spans;
      spans.reserve(kNbSpans);
      for (std::size_t pos = 0; pos < kNbSpans; ++pos) {
        spans.push_back(recorder->createSpan("span." + std::to_string(pos)));
        spans.back()->setAttribute("pos", static_cast<int64_t>(pos));
      }
      // Released in reverse order.
      while (!spans.empty()) {
        spans.pop_back();
      }
    }
  }
  ASSERT_EQ(state.batches.size(), 1U);
  ASSERT_EQ(state.batchSpans[0], 2 * kNbSpans);
  const ExportTraceServiceRequest request = ParseBatch(state.batches[0]);
  const auto& spans = SpansOf(request);
  ASSERT_EQ(static_cast<std::size_t>(spans.size()), 2 * kNbSpans);
  for (std::size_t pos = 0; pos < 2 * kNbSpans; ++pos) {
    const std::size_t spanPos = kNbSpans - 1 - (pos % kNbSpans);
    const OtlpSpan& span = spans[static_cast<int>(pos)];
    EXPECT_EQ(span.name(), "span." + std::to_string(spanPos));
    ASSERT_EQ(span.attributes_size(), 1);
    EXPECT_EQ(span.attributes(0).value().int_value(), static_cast<int64_t>(spanPos));
  }
}

// A value that is not UTF-8 is sent as bytes: in a string, it would make the collector reject the whole request.
TEST(SpanRecorder, InvalidUtf8ValuesAreBytes) {
  static constexpr std::string_view kInvalidValues[]{
      "\xFF", "a\xC0\x80", "\xE2\x82", "\xED\xA0\x80", "\xF4\x90\x80\x80", "path/\x80/x",
  };
  static constexpr std::string_view kValidValue = "\xE2\x82\xAC";
  SinkState state;
  {
    const std::unique_ptr<SpanRecorder> recorder = MakeRecorder(state);
    const SpanPtr span = recorder->createSpan("utf8");
    for (const std::string_view value : kInvalidValues) {
      span->setAttribute("invalid", value);
    }
    span->setAttribute("valid", kValidValue);
  }
  ASSERT_EQ(state.batches.size(), 1U);
  const ExportTraceServiceRequest request = ParseBatch(state.batches[0]);
  const OtlpSpan& span = SpansOf(request)[0];
  ASSERT_EQ(static_cast<std::size_t>(span.attributes_size()), std::size(kInvalidValues) + 1);
  for (std::size_t pos = 0; pos < std::size(kInvalidValues); ++pos) {
    const auto& value = span.attributes(static_cast<int>(pos)).value();
    EXPECT_TRUE(value.has_bytes_value()) << pos;
    EXPECT_EQ(value.bytes_value(), kInvalidValues[pos]);
  }
  const auto& validValue = span.attributes(static_cast<int>(std::size(kInvalidValues))).value();
  EXPECT_TRUE(validValue.has_string_value());
  EXPECT_EQ(validValue.string_value(), kValidValue);
}

TEST(SpanRecorder, EndAndAttributesAfterEndAreIgnored) {
  SinkState state;
  const std::vector<TestSpan> spans{{"ended", {{"before", int64_t{1}}}}};
  {
    const std::unique_ptr<SpanRecorder> recorder = MakeRecorder(state);
    const SpanPtr span = recorder->createSpan("ended");
    span->setAttribute("before", int64_t{1});
    span->end();
    span->setAttribute("after", int64_t{2});
    span->setAttribute("after", "value");
    span->end();
  }
  ASSERT_EQ(state.batches.size(), 1U);
  const ExportTraceServiceRequest actual = ParseBatch(state.batches[0]);
  ExpectEqualRequests(SdkRequest(actual, spans), actual);
}

// As the SDK, which ends a span destroyed without End().
TEST(SpanRecorder, SpanRecycledWithoutEndIsEnded) {
  SinkState state;
  const std::vector<TestSpan> spans{{"not-ended", {{"key", "value"}}}};
  {
    const std::unique_ptr<SpanRecorder> recorder = MakeRecorder(state);
    const SpanPtr span = recorder->createSpan("not-ended");
    span->setAttribute("key", "value");
  }
  ASSERT_EQ(state.batches.size(), 1U);
  const ExportTraceServiceRequest actual = ParseBatch(state.batches[0]);
  ExpectEqualRequests(SdkRequest(actual, spans), actual);
  EXPECT_NE(SpansOf(actual)[0].end_time_unix_nano(), 0U);
}

TEST(SpanRecorder, SpansHaveDistinctIdsAndTheirSystemTimes) {
  static constexpr int kNbSpans = 100;
  SinkState state;
  const auto before = std::chrono::system_clock::now();
  {
    const std::unique_ptr<SpanRecorder> recorder = MakeRecorder(state);
    for (int pos = 0; pos < kNbSpans; ++pos) {
      recorder->createSpan("span")->end();
    }
  }
  const auto after = std::chrono::system_clock::now();
  ASSERT_EQ(state.batches.size(), 1U);
  const ExportTraceServiceRequest request = ParseBatch(state.batches[0]);
  const auto toNs = [](std::chrono::system_clock::time_point timePoint) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(timePoint.time_since_epoch()).count());
  };
  // The system clock is read once per batch: allow it to move by a millisecond meanwhile.
  const uint64_t minNs = toNs(before - 1ms);
  const uint64_t maxNs = toNs(after + 1ms);
  std::set<std::string> traceIds;
  std::set<std::string> spanIds;
  for (const OtlpSpan& span : SpansOf(request)) {
    EXPECT_TRUE(traceIds.insert(span.trace_id()).second);
    EXPECT_TRUE(spanIds.insert(span.span_id()).second);
    EXPECT_NE(span.trace_id(), std::string(16, '\0'));
    EXPECT_NE(span.span_id(), std::string(8, '\0'));
    EXPECT_TRUE(span.parent_span_id().empty());
    EXPECT_EQ(span.flags(), 1U);
    EXPECT_EQ(span.kind(), opentelemetry::proto::trace::v1::Span::SPAN_KIND_INTERNAL);
    EXPECT_GE(span.start_time_unix_nano(), minNs);
    EXPECT_LE(span.start_time_unix_nano(), span.end_time_unix_nano());
    EXPECT_LE(span.end_time_unix_nano(), maxNs);
  }
  EXPECT_EQ(traceIds.size(), static_cast<std::size_t>(kNbSpans));
}

TEST(SpanRecorder, BatchIsHandedOffAtItsLastSpan) {
  SinkState state;
  const std::unique_ptr<SpanRecorder> recorder = MakeRecorder(state);
  for (uint32_t pos = 0; pos + 1 < SpanRecorder::kSpansPerBatch; ++pos) {
    recorder->createSpan("span")->end();
  }
  recorder->flush();  // exportInterval not passed
  {
    const std::scoped_lock lock(state.mutex);
    EXPECT_EQ(state.nbSendsStarted, 0);
  }
  recorder->createSpan("span")->end();
  ASSERT_TRUE(WaitForBatches(state, 1));
  EXPECT_EQ(state.batchSpans[0], SpanRecorder::kSpansPerBatch);
  EXPECT_EQ(SpansOf(ParseBatch(state.batches[0])).size(), static_cast<int>(SpanRecorder::kSpansPerBatch));
}

TEST(SpanRecorder, FlushHandsOffOnceTheExportIntervalPassed) {
  SinkState state;
  const std::unique_ptr<SpanRecorder> recorder = MakeRecorder(state, 0ms);
  recorder->flush();  // no span: no batch
  for (int pos = 0; pos < 3; ++pos) {
    recorder->createSpan("span")->end();
  }
  recorder->flush();
  ASSERT_TRUE(WaitForBatches(state, 1));
  EXPECT_EQ(state.batchSpans[0], 3U);
  EXPECT_EQ(SpansOf(ParseBatch(state.batches[0])).size(), 3);
}

TEST(SpanRecorder, DestructionExportsTheQueuedSpans) {
  SinkState state;
  {
    const std::unique_ptr<SpanRecorder> recorder = MakeRecorder(state);
    for (int pos = 0; pos < 10; ++pos) {
      recorder->createSpan("span")->end();
    }
    recorder->flush();  // exportInterval not passed
  }
  ASSERT_EQ(state.batches.size(), 1U);
  EXPECT_EQ(state.batchSpans[0], 10U);
  EXPECT_TRUE(state.shutdown);
}

TEST(SpanRecorder, DestructionWithoutSpans) {
  SinkState state;
  MakeRecorder(state).reset();
  EXPECT_TRUE(state.batches.empty());
  EXPECT_TRUE(state.shutdown);
}

// While the exporter is busy (here with a collector that does not answer), the batch waiting for it and one queue of
// kMaxQueuedSpans spans are kept, the next spans dropped.
TEST(SpanRecorder, DropsTheSpansBeyondTheQueueWhileTheExporterIsBusy) {
  static constexpr uint32_t kNbDropped = 100;
  SinkState state;
  state.stalled = true;
  {
    const std::unique_ptr<SpanRecorder> recorder = MakeRecorder(state);
    for (uint32_t pos = 0; pos < SpanRecorder::kSpansPerBatch; ++pos) {
      recorder->createSpan("span")->end();
    }
    ASSERT_TRUE(WaitForSendsStarted(state, 1));
    // The second batch waits for the thread, the next spans queue.
    for (uint32_t pos = 0; pos < SpanRecorder::kSpansPerBatch + SpanRecorder::kMaxQueuedSpans + kNbDropped; ++pos) {
      recorder->createSpan("span")->end();
    }
    {
      const std::scoped_lock lock(state.mutex);
      state.stalled = false;
    }
    state.cv.notify_all();
  }
  const std::vector<uint32_t> expectedBatchSpans{
      SpanRecorder::kSpansPerBatch,
      SpanRecorder::kSpansPerBatch,
      SpanRecorder::kMaxQueuedSpans,
  };
  EXPECT_EQ(state.batchSpans, expectedBatchSpans);
  ASSERT_EQ(state.batches.size(), expectedBatchSpans.size());
  for (std::size_t pos = 0; pos < expectedBatchSpans.size(); ++pos) {
    EXPECT_EQ(SpansOf(ParseBatch(state.batches[pos])).size(), static_cast<int>(expectedBatchSpans[pos]));
  }
}

TEST(SpanBatchExporter, StopsAtDestruction) {
  SinkState state;
  {
    const SpanBatchExporter exporter(std::make_unique<TestSink>(state), MakeOtlpSpanBatchHeader(TestResource()));
  }
  EXPECT_TRUE(state.batches.empty());
  EXPECT_TRUE(state.shutdown);
}

// The header with schema URLs, which the resource and scope of aeronet do not have.
TEST(SpanBatchHeader, SchemaUrls) {
  opentelemetry::proto::trace::v1::ResourceSpans expected;
  expected.mutable_resource()->set_dropped_attributes_count(3);
  expected.set_schema_url("https://resource.schema");
  auto* pScopeSpans = expected.add_scope_spans();
  pScopeSpans->mutable_scope()->set_name("scope");
  pScopeSpans->set_schema_url("https://scope.schema");
  pScopeSpans->add_spans()->set_name("span");

  const SpanBatchHeader header(expected.resource().SerializeAsString(), expected.schema_url(),
                               pScopeSpans->scope().SerializeAsString(), pScopeSpans->schema_url());
  const std::string spanBytes = pScopeSpans->spans(0).SerializeAsString();
  RawChars32 batch = header.emptyBatch();
  batch.push_back(static_cast<char>((2U << 3U) | 2U));  // ScopeSpans.spans
  batch.push_back(static_cast<char>(spanBytes.size()));
  batch.append(spanBytes);
  header.writeTo(batch);

  opentelemetry::proto::trace::v1::ResourceSpans actual;
  ASSERT_TRUE(actual.ParseFromArray(batch.data(), static_cast<int>(batch.size())));
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(expected, actual)) << actual.DebugString();
}

}  // namespace
}  // namespace aeronet::tracing::detail
