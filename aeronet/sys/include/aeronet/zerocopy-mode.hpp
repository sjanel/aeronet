#pragma once

#include <cstdint>

namespace aeronet {

// MSG_ZEROCOPY mode for the large response writes of plain (non-TLS) HTTP/1.1 connections (Linux-only, with automatic
// fallback to regular copying writes).
// With MSG_ZEROCOPY, the kernel sends the response bytes from their user-space pages instead of copying them into the
// socket buffer. These bytes are kept alive (and unchanged) until the kernel reports the completion of the send, which
// happens once the peer acknowledged them.
//
// Benefits:
//   - Reduces CPU overhead for large sends (no copy into kernel buffers)
//   - Better memory bandwidth utilization (less L3/memory controller pressure)
//
// Requirements and limits:
//   - Linux 4.14+ with SO_ZEROCOPY socket support
//   - Writes of at least HttpServerConfig::zerocopyMinBytes (smaller ones are copied: pinning pages costs more)
//   - The kernel copies the data anyway for a local peer (loopback, another network namespace of the host) or a device
//     without scatter-gather support
//   - Each zerocopy send pins the pages it reads, accounted to the locked memory limit of the user (RLIMIT_MEMLOCK,
//     unless CAP_IPC_LOCK) and to the socket option memory (net.core.optmem_max): beyond them, writes are copied
//   - Not used with TLS (kTLS sockets reject MSG_ZEROCOPY; user-space TLS copies the data while encrypting it), nor for
//     HTTP/2, WebSocket or CONNECT tunnels, whose output buffers are reused as soon as they are written
enum class ZerocopyMode : std::uint8_t {
  // Never use MSG_ZEROCOPY
  Disabled,

  // Use MSG_ZEROCOPY when available, except for loopback peers, and stop using it on a connection once the kernel
  // reports that it copied the data anyway (local peer, device without scatter-gather) (DEFAULT)
  Opportunistic,

  // Use MSG_ZEROCOPY when available, even for loopback peers or when the kernel copies the data anyway - logs a warning
  // if the kernel does not support it. It is still not used for payloads below the threshold.
  Enabled,
};

}  // namespace aeronet

#ifdef AERONET_ENABLE_GLAZE
#include <glaze/glaze.hpp>

template <>
struct glz::meta<aeronet::ZerocopyMode> {
  using enum aeronet::ZerocopyMode;
  static constexpr auto value = enumerate("disabled", Disabled, "opportunistic", Opportunistic, "enabled", Enabled);
};
#endif