#pragma once

#include <gtest/gtest.h>

#include <string>

#include "aeronet/chrono-lite.hpp"

namespace aeronet::test {

// Result of one attempt of a timing-sensitive scenario.
struct StallOutcome {
  bool ok{false};
  bool stalled{false};  // failure attributable to the runner (client thread not scheduled), not to the server
  std::string detail;
};

// Runs a timing-sensitive scenario with a short keepAliveTimeout, doubling it after each attempt that was disturbed by
// the runner. A failure that happened while the client was prompt is a real failure and is never retried.
template <class Scenario>
void RunStallTolerant(Scenario&& scenario) {
  std::chrono::milliseconds timeout{100};
  StallOutcome last;
  for (int attempt = 0; attempt < 4; ++attempt, timeout *= 2) {
    last = scenario(timeout);
    if (last.ok) {
      return;
    }
    if (!last.stalled) {
      FAIL() << last.detail;
    }
  }
  GTEST_SKIP() << "runner too loaded, every attempt was stalled; last: " << last.detail;
}

}  // namespace aeronet::test
