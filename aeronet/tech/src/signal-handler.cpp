#include "aeronet/signal-handler.hpp"

#include <atomic>
#include <chrono>
#include <csignal>

#include "aeronet/log.hpp"

namespace {

// Written by the signal handler, read by the event loops of any thread: a lock-free atomic is both async-signal-safe
// and visible across threads (unlike a volatile std::sig_atomic_t, which is only meant for the interrupted thread).
std::atomic<int> g_signalStatus{0};
static_assert(std::atomic<int>::is_always_lock_free);

std::atomic<bool> g_stopRequestLogged{false};
std::atomic<std::chrono::milliseconds::rep> g_maxDrainPeriodMs{5000};

}  // namespace

extern "C" void AeronetSignalHandler(int sigNum) {
  // Only async-signal-safe operations here: logging (formatting, allocating, locking) could deadlock if the signal
  // interrupted a thread holding the same locks. The stop request is logged by IsStopRequested() instead.
  g_signalStatus.store(sigNum, std::memory_order_relaxed);
}

namespace aeronet {

void SignalHandler::Enable(std::chrono::milliseconds maxDrainPeriod) {
  g_maxDrainPeriodMs.store(maxDrainPeriod.count(), std::memory_order_relaxed);
  std::signal(SIGINT, ::AeronetSignalHandler);
  std::signal(SIGTERM, ::AeronetSignalHandler);
}

void SignalHandler::Disable() {
  std::signal(SIGINT, SIG_DFL);
  std::signal(SIGTERM, SIG_DFL);
}

bool SignalHandler::IsStopRequested() {
  const int sigNum = g_signalStatus.load(std::memory_order_relaxed);
  if (sigNum == 0) {
    return false;
  }
  if (!g_stopRequestLogged.load(std::memory_order_relaxed) &&
      !g_stopRequestLogged.exchange(true, std::memory_order_relaxed)) {
    log::warn("Signal {} received, gracefully shutting down with a max drain period of {}ms", sigNum,
              g_maxDrainPeriodMs.load(std::memory_order_relaxed));
  }
  return true;
}

std::chrono::milliseconds SignalHandler::GetMaxDrainPeriod() {
  return std::chrono::milliseconds{g_maxDrainPeriodMs.load(std::memory_order_relaxed)};
}

void SignalHandler::ResetStopRequest() {
  g_signalStatus.store(0, std::memory_order_relaxed);
  g_stopRequestLogged.store(false, std::memory_order_relaxed);
}

}  // namespace aeronet
