#include "aeronet/signal-handler.hpp"

#include <chrono>
#include <csignal>

#include "aeronet/log.hpp"

namespace {

volatile std::sig_atomic_t g_signalStatus{};
constinit std::chrono::milliseconds::rep g_maxDrainPeriodMs{5000};

}  // namespace

extern "C" void AeronetSignalHandler(int sigNum) {
  ::aeronet::log::warn("Signal {} received, gracefully shutting down with a max drain period of {}ms", sigNum,
                       g_maxDrainPeriodMs);

  g_signalStatus = sigNum;
}

namespace aeronet {

void SignalHandler::Enable(std::chrono::milliseconds maxDrainPeriod) {
  std::signal(SIGINT, ::AeronetSignalHandler);
  std::signal(SIGTERM, ::AeronetSignalHandler);
  g_maxDrainPeriodMs = maxDrainPeriod.count();
}

void SignalHandler::Disable() {
  std::signal(SIGINT, SIG_DFL);
  std::signal(SIGTERM, SIG_DFL);
}

bool SignalHandler::IsStopRequested() { return g_signalStatus != 0; }

std::chrono::milliseconds SignalHandler::GetMaxDrainPeriod() { return std::chrono::milliseconds{g_maxDrainPeriodMs}; }

void SignalHandler::ResetStopRequest() { g_signalStatus = 0; }

}  // namespace aeronet
