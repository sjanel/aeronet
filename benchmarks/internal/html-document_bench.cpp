// Cost of the HTML module on a page made of a table of 'rows' rows (about 110 bytes of markup per row):
//   parsing (UTF-8 given by the Content-Type, or windows-1252 decoded to UTF-8 first),
//   selector queries (selector given as text, or compiled once as CssSelector; vector result or forEach()),
//   textContent() and innerHtml() of the rows and cells (new std::string each time, or a reused buffer), and the search
//   of a JSON state assigned by an inline script.
// Also the parsing of hostile shapes, kept linear by the limits of the tree construction (src/html-parse.hpp).
#include <benchmark/benchmark.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "aeronet/css-selector.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/html-element.hpp"

namespace aeronet {

namespace {

constexpr std::string_view kHead = "<!DOCTYPE html><html><head><title>Fees</title></head><body><table class=\"fees\">";
constexpr std::string_view kRowStart = R"(<tr class="row"><td class="name"><a href="/asset/)";
constexpr std::string_view kAfterHref = R"(">Asset )";
constexpr std::string_view kMidCell = "</a></td><td>";
constexpr std::string_view kRowEnd = "</td></tr>";
constexpr std::string_view kFoot = "</table></body></html>";

// Sum of the decimal digit counts of 0, 1, ..., n-1, in O(log n).
size_t TotalDigits(uint64_t n) {
  size_t total = 0;
  uint64_t lo = 0, hi = 10;  // numbers with d digits are [lo, hi), except d=1 which includes 0
  for (size_t d = 1; lo < n; ++d) {
    total += static_cast<size_t>(std::min(n, hi) - lo) * d;
    lo = hi;
    hi *= 10;  // may wrap on the last iteration, but then lo >= n and we stop
  }
  return total;
}

inline char* Put(char* p, std::string_view s) {
  std::memcpy(p, s.data(), s.size());
  return p + s.size();
}

// A page with a table of 'rows' rows. 'cellText' is the text of the second cell of each row.
std::string MakeTablePage(int64_t rows, std::string_view cellText) {
  const uint64_t n = rows > 0 ? static_cast<uint64_t>(rows) : 0;

  const std::size_t perRowFixed =
      kRowStart.size() + kAfterHref.size() + kMidCell.size() + cellText.size() + kRowEnd.size();
  const std::size_t total = kHead.size() + kFoot.size() + n * perRowFixed + 2 * TotalDigits(n);

  std::string html;
  html.resize_and_overwrite(total, [&](char* buf, std::size_t) {
    char* const end = buf + total;
    char* p = Put(buf, kHead);
    for (uint64_t row = 0; row < n; ++row) {
      p = Put(p, kRowStart);

      // Format the number directly into the output, remember where it landed.
      char* const num = p;
      p = std::to_chars(p, end, row).ptr;
      const std::size_t len = static_cast<std::size_t>(p - num);

      p = Put(p, kAfterHref);

      // Second occurrence: copy the digits we just wrote (source precedes dest, no overlap).
      std::memcpy(p, num, len);
      p += len;

      p = Put(p, kMidCell);
      p = Put(p, cellText);
      p = Put(p, kRowEnd);
    }
    p = Put(p, kFoot);
    return static_cast<std::size_t>(p - buf);  // == total
  });
  return html;
}

void BM_ParseUtf8(benchmark::State& state) {
  const std::string html = MakeTablePage(state.range(0), "6.00815 ZRX");
  for ([[maybe_unused]] auto _ : state) {
    HtmlDocument doc(html, "text/html; charset=utf-8");
    benchmark::DoNotOptimize(doc);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(html.size()));
}

// Each cell has 3 non-ASCII characters ("été" in windows-1252).
void BM_ParseWindows1252(benchmark::State& state) {
  const std::string html = MakeTablePage(state.range(0), "\xE9t\xE9 6.00815");
  for ([[maybe_unused]] auto _ : state) {
    HtmlDocument doc(html, "text/html; charset=windows-1252");
    benchmark::DoNotOptimize(doc);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(html.size()));
}

// Same document without declared encoding: validated as UTF-8 first.
void BM_ParseUndeclaredUtf8(benchmark::State& state) {
  const std::string html = MakeTablePage(state.range(0), "\xC3\xA9t\xC3\xA9 6.00815");
  for ([[maybe_unused]] auto _ : state) {
    HtmlDocument doc(html);
    benchmark::DoNotOptimize(doc);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(html.size()));
}

// 'range' nested <div> elements, beyond the depth limit (512).
void BM_ParseNestedElements(benchmark::State& state) {
  std::string html;
  for (int64_t idx = 0; idx < state.range(0); ++idx) {
    html += "<div>";
  }
  for ([[maybe_unused]] auto _ : state) {
    HtmlDocument doc(html, "text/html; charset=utf-8");
    benchmark::DoNotOptimize(doc);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(html.size()));
}

// 'range' paragraphs, each leaving open a <b> of a different id: the text of each one recreates the previous ones, up
// to the limit of active formatting elements (16).
void BM_ParseMisnestedFormatting(benchmark::State& state) {
  std::string html;
  for (int64_t idx = 0; idx < state.range(0); ++idx) {
    html += "<p><b id=" + std::to_string(idx) + ">x</p>";
  }
  for ([[maybe_unused]] auto _ : state) {
    HtmlDocument doc(html, "text/html; charset=utf-8");
    benchmark::DoNotOptimize(doc);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(html.size()));
}

constexpr std::string_view kCellSelector = "table.fees tr.row > td.name";

void BM_QueryAllText(benchmark::State& state) {
  const HtmlDocument doc(MakeTablePage(state.range(0), "6.00815 ZRX"));
  for ([[maybe_unused]] auto _ : state) {
    auto cells = doc.querySelectorAll(kCellSelector);
    benchmark::DoNotOptimize(cells);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_QueryAllCompiled(benchmark::State& state) {
  const HtmlDocument doc(MakeTablePage(state.range(0), "6.00815 ZRX"));
  const CssSelector selector(kCellSelector);
  for ([[maybe_unused]] auto _ : state) {
    auto cells = doc.querySelectorAll(selector);
    benchmark::DoNotOptimize(cells);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_ForEachCompiled(benchmark::State& state) {
  const HtmlDocument doc(MakeTablePage(state.range(0), "6.00815 ZRX"));
  const CssSelector selector(kCellSelector);
  for ([[maybe_unused]] auto _ : state) {
    std::size_t count = 0;
    doc.forEach(selector, [&count](HtmlElement) { ++count; });
    benchmark::DoNotOptimize(count);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// The first match: the search stops there.
void BM_QueryFirstText(benchmark::State& state) {
  const HtmlDocument doc(MakeTablePage(state.range(0), "6.00815 ZRX"));
  for ([[maybe_unused]] auto _ : state) {
    HtmlElement cell = doc.querySelector(kCellSelector);
    benchmark::DoNotOptimize(cell);
  }
}

void BM_TextContent(benchmark::State& state) {
  const HtmlDocument doc(MakeTablePage(state.range(0), "6.00815 ZRX"));
  const auto rows = doc.querySelectorAll("tr");
  for ([[maybe_unused]] auto _ : state) {
    std::size_t size = 0;
    for (HtmlElement row : rows) {
      size += row.textContent().size();
    }
    benchmark::DoNotOptimize(size);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// The text of a row is in several nodes: copied to the reused buffer.
void BM_TextContentBuffer(benchmark::State& state) {
  const HtmlDocument doc(MakeTablePage(state.range(0), "6.00815 ZRX"));
  const auto rows = doc.querySelectorAll("tr");
  std::string buffer;
  for ([[maybe_unused]] auto _ : state) {
    std::size_t size = 0;
    for (HtmlElement row : rows) {
      size += row.textContent(buffer).size();
    }
    benchmark::DoNotOptimize(size);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// The text of each cell is in a single node, longer than the inline capacity of std::string (15 bytes in libstdc++).
constexpr std::string_view kLongCellText = "6.00815 ZRX - 0x Protocol Token";

void BM_CellTextContent(benchmark::State& state) {
  const HtmlDocument doc(MakeTablePage(state.range(0), kLongCellText));
  const auto cells = doc.querySelectorAll("td");
  for ([[maybe_unused]] auto _ : state) {
    std::size_t size = 0;
    for (HtmlElement cell : cells) {
      size += cell.textContent().size();
    }
    benchmark::DoNotOptimize(size);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(cells.size()));
}

// Same cells, without copy: the views point into the document.
void BM_CellTextContentBuffer(benchmark::State& state) {
  const HtmlDocument doc(MakeTablePage(state.range(0), kLongCellText));
  const auto cells = doc.querySelectorAll("td");
  std::string buffer;
  for ([[maybe_unused]] auto _ : state) {
    std::size_t size = 0;
    for (HtmlElement cell : cells) {
      size += cell.textContent(buffer).size();
    }
    benchmark::DoNotOptimize(size);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(cells.size()));
}

void BM_InnerHtml(benchmark::State& state) {
  const HtmlDocument doc(MakeTablePage(state.range(0), "6.00815 ZRX"));
  const auto rows = doc.querySelectorAll("tr");
  for ([[maybe_unused]] auto _ : state) {
    std::size_t size = 0;
    for (HtmlElement row : rows) {
      size += row.innerHtml().size();
    }
    benchmark::DoNotOptimize(size);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// The serialization of each row appended to a reused buffer.
void BM_AppendInnerHtml(benchmark::State& state) {
  const HtmlDocument doc(MakeTablePage(state.range(0), "6.00815 ZRX"));
  const auto rows = doc.querySelectorAll("tr");
  std::string buffer;
  for ([[maybe_unused]] auto _ : state) {
    std::size_t size = 0;
    for (HtmlElement row : rows) {
      buffer.clear();
      row.appendInnerHtml(buffer);
      size += buffer.size();
    }
    benchmark::DoNotOptimize(size);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// A JSON state of 'range' entries (about 60 bytes each) assigned by the last of a few scripts.
void BM_AssignedJson(benchmark::State& state) {
  std::string html =
      "<script>window.dataLayer = window.dataLayer || [];</script><script>window.__INITIAL_PROPS__={\"q\":[";
  for (int64_t entry = 0; entry < state.range(0); ++entry) {
    html += R"({"asset":"A)" + std::to_string(entry) + R"(","fee":"6.00815","min":"7.2 {[]}"},)";
  }
  html += "{}]};</script>";
  const HtmlDocument doc(html);
  int64_t jsonSize = 0;
  const HtmlDocument* pDoc = &doc;
  for ([[maybe_unused]] auto _ : state) {
    // The document escapes at each iteration: the search cannot be hoisted out of the loop.
    benchmark::DoNotOptimize(pDoc);
    std::string_view json = pDoc->assignedJson("window.__INITIAL_PROPS__");
    jsonSize = static_cast<int64_t>(json.size());
    benchmark::DoNotOptimize(json);
  }
  state.SetBytesProcessed(state.iterations() * jsonSize);
}

}  // namespace

BENCHMARK(BM_ParseUtf8)->Arg(10)->Arg(1000)->Arg(10000);
BENCHMARK(BM_ParseWindows1252)->Arg(10)->Arg(1000)->Arg(10000);
BENCHMARK(BM_ParseUndeclaredUtf8)->Arg(10)->Arg(1000)->Arg(10000);
BENCHMARK(BM_ParseNestedElements)->Arg(1000)->Arg(100000);
BENCHMARK(BM_ParseMisnestedFormatting)->Arg(1000)->Arg(50000);
BENCHMARK(BM_QueryAllText)->Arg(10)->Arg(1000)->Arg(10000);
BENCHMARK(BM_QueryAllCompiled)->Arg(10)->Arg(1000)->Arg(10000);
BENCHMARK(BM_ForEachCompiled)->Arg(10)->Arg(1000)->Arg(10000);
BENCHMARK(BM_QueryFirstText)->Arg(1000);
BENCHMARK(BM_TextContent)->Arg(1000);
BENCHMARK(BM_TextContentBuffer)->Arg(1000);
BENCHMARK(BM_CellTextContent)->Arg(1000);
BENCHMARK(BM_CellTextContentBuffer)->Arg(1000);
BENCHMARK(BM_InnerHtml)->Arg(1000);
BENCHMARK(BM_AppendInnerHtml)->Arg(1000);
BENCHMARK(BM_AssignedJson)->Arg(100)->Arg(10000);

}  // namespace aeronet

BENCHMARK_MAIN();
