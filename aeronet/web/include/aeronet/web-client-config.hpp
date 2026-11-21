#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "aeronet/http-client-config.hpp"

namespace aeronet {

// Configuration of a WebClient. It is an aggregate, reflected by glaze (see web-client-config-glaze.hpp).
// The limits bound what the scripts of a page can do in the process: std::numeric_limits<T>::max() means no limit.
struct WebClientConfig {
  // Throws std::invalid_argument if a field is invalid: a zero memory or stack limit, a negative maxTimerDelay, or an
  // invalid http configuration.
  void validate() const;

  // Configuration of the HTTP client that fetches the documents and their scripts. Its cookies are always enabled,
  // with the default limits of HttpClientConfig::Cookies when http.cookies is disabled: pages expect them.
  HttpClientConfig http{};

  // Maximum memory of the JavaScript heap of a page, in bytes, and apart, of the native memory that its scripts make it
  // hold (the DOM content they add, the output of document.write(), their timers): an allocation beyond it throws an
  // out of memory error in the script.
  std::size_t maxScriptMemory{256UL << 20U};

  // Maximum native stack that the scripts of a page use, in bytes: a deeper recursion throws a RangeError. It must
  // stay below the free stack of the thread that calls WebClient::navigate() and WebPage::evaluate() (the main thread
  // of a Windows executable has 1 MiB, unless linked with /STACK).
  std::size_t maxScriptStackSize{1UL << 20U};

  // Maximum total size of the scripts (inline and external) that a document runs, in bytes: the next ones are skipped
  // (0 runs none).
  std::size_t maxScriptBytes{32UL << 20U};

  // Horizon of the virtual clock of the timers of a document: a timer due later than this after the start of the
  // document never runs.
  std::chrono::milliseconds maxTimerDelay{std::chrono::minutes{1}};

  // Maximum size of the bytecode of the scripts that a WebClient keeps for its next pages, in bytes: a script run again
  // (same URL and content, at least 1 KiB) is read back instead of compiled, 15 to 70 times faster. The least recently
  // used scripts are dropped beyond it (0 keeps none).
  std::size_t maxScriptCacheBytes{64UL << 20U};

  // Maximum number of runs of a setInterval() callback, after which the interval is cleared.
  uint32_t maxIntervalRuns{100};

  // Maximum number of requests (fetch(), XMLHttpRequest, navigator.sendBeacon()) that the scripts of a document make:
  // the next ones fail as network errors.
  uint32_t maxRequests{1000};

  // Maximum number of navigations started by a page (location assignments, <meta http-equiv="refresh">) in one
  // WebClient::navigate() call, beyond which it fails with WebErrc::tooManyNavigations. HTTP redirects are not
  // counted: HttpClientConfig::maxRedirects limits them.
  uint32_t maxNavigations{20};
};

}  // namespace aeronet
