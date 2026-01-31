#pragma once

#include <cstdint>

namespace aeronet {

// MSG_ZEROCOPY mode for outbound writes on TCP sockets (Linux-only, with automatic fallback).
// When enabled, large payloads (>= 16KB) are sent via the kernel's zerocopy path, avoiding a memcpy from
// user-space to kernel buffers. The kernel DMA-maps the user pages directly to the NIC.
// By default, zerocopy is used opportunistically on non-loopback connections only.
//
// Benefits:
//   - Reduces CPU overhead for large sends (10-30% CPU savings on egress-heavy workloads)
//   - Better memory bandwidth utilization (less L3/memory controller pressure)
//   - Higher sustained throughput for streaming responses and file transfers
//
// Requirements:
//   - Linux 4.14+ with SO_ZEROCOPY socket support
//   - Payloads >= 16KB (smaller payloads use regular send to avoid page-pinning overhead)
//   - TCP sockets (not UDP)
//
// For TLS: zerocopy is only used when kTLS (kernel TLS) is active because the kernel can encrypt and
// DMA from pinned pages. With user-space TLS (OpenSSL), zerocopy has no benefit as data is already
// copied during encryption.
enum class ZerocopyMode : std::uint8_t {
  // Never use MSG_ZEROCOPY
  Disabled,

  // Use MSG_ZEROCOPY if available for plain TCP and kTLS connections, only on non-loopback connections (DEFAULT)
  Opportunistic,

  // Enables zerocopy even if connection is on loopback - logs a warning if zerocopy cannot be activated.
  // It will still not be used for payloads below the threshold.
  Enabled
};

}  // namespace aeronet