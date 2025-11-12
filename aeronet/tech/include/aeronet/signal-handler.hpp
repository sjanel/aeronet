#pragma once

#include <chrono>

namespace aeronet {

class SignalHandler {
 public:
  SignalHandler() noexcept = delete;

  // Sets up signal handlers for SIGINT and SIGTERM to request graceful shutdown.
  // maxDrainPeriod specifies the maximum time to allow for graceful draining
  // after a termination signal is received (default: 5000 ms, 0: no limit).
  static void Enable(std::chrono::milliseconds maxDrainPeriod = std::chrono::milliseconds{5000});

  // Disables the signal handlers and restores default behavior.
  static void Disable();

  // Returns true if a termination signal was received.
  static bool IsStopRequested();

  // Returns the maximum drain period configured for signal handling.
  static std::chrono::milliseconds GetMaxDrainPeriod();

 private:
  friend class SignalHandlerGlobalTest;

  // Resets the stop-requested flag (for testing purposes).
  // This allows multiple test runs in the same process.
  static void ResetStopRequest();
};

}  // namespace aeronet
