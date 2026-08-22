#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "aeronet/native-handle.hpp"
#include "aeronet/zerocopy-mode.hpp"
#include "aeronet/zerocopy.hpp"

namespace aeronet {

class File;

// Indicates what the transport layer needs to proceed after a non-blocking I/O operation returns EAGAIN/WANT.
enum class TransportHint : uint8_t {
  None,        // No special action needed (operation completed or fatal error)
  ReadReady,   // Need socket readable before operation can proceed (SSL_ERROR_WANT_READ)
  WriteReady,  // Need socket writable before operation can proceed (SSL_ERROR_WANT_WRITE)
  Error,
};

struct TransportResult {
  std::size_t bytesProcessed;  // bytes read for read operations, or written for write operations
  TransportHint want;          // socket readiness needed before the operation can proceed
};

enum class TransportKind : uint8_t { Empty, Plain, Tls, Custom };

/// Shared non-virtual state for socket-backed transports.
class SocketTransportState {
 public:
  /// Poll for zerocopy completion notifications from the kernel error queue.
  std::size_t pollZerocopyCompletions() noexcept { return PollZeroCopyCompletions(_fd, _zerocopyState); }

  /// Check if zerocopy is enabled on this transport.
  [[nodiscard]] bool isZerocopyEnabled() const noexcept { return _zerocopyState.enabled(); }

  /// Check if any zerocopy sends are waiting for completion.
  [[nodiscard]] bool hasZerocopyPending() const noexcept { return _zerocopyState.pendingCompletions(); }

  /// Disable zerocopy for this transport.
  void disableZerocopy() noexcept { _zerocopyState.setEnabled(false); }

 protected:
  SocketTransportState() noexcept = default;

  SocketTransportState(NativeHandle fd, std::uint32_t minBytesForZerocopy)
      : _minBytesForZerocopy(minBytesForZerocopy), _fd(fd) {}

  ZeroCopyState _zerocopyState{};
  std::uint32_t _minBytesForZerocopy{~0U};
  NativeHandle _fd{kInvalidHandle};
};

// Plain transport directly operates on a non-blocking fd.
// Supports optional MSG_ZEROCOPY for large payloads on Linux.
class PlainTransport final : public SocketTransportState {
 public:
  using TransportResult = aeronet::TransportResult;

  PlainTransport(NativeHandle fd, ZerocopyMode zerocopyMode, uint32_t minBytesForZerocopy);

  TransportResult read(char* buf, std::size_t len);

  TransportResult write(std::string_view data);

  /// Scatter write using writev - single syscall for two buffers.
  TransportResult write(std::string_view firstBuf, std::string_view secondBuf);

  /// Gather write using writev / WSASend.
  TransportResult write(std::span<const std::string_view> buffers);

  /// Zero-copy sendfile(2) of a file region straight to the socket (no user-space copy).
  TransportResult sendFile(const File& file, std::size_t& offset, std::size_t count);
};

class ErasedTransportBackend {
 protected:
  explicit ErasedTransportBackend(const void* operations) noexcept : _operations(operations) {}

 private:
  friend class Transport;

  const void* _operations;
};

template <typename Backend, TransportKind Kind = TransportKind::Custom>
class TransportBackend;

/// Final owning transport with inline plain storage and erased TLS/custom dispatch.
///
/// Plain I/O is stored directly in this object and takes a predictable variant-index branch. TLS and custom/test
/// backends remain out of line behind one cached operation table per concrete type, keeping OpenSSL out of this module
/// and preserving optional TLS builds.
class Transport final {
 public:
  Transport() noexcept = default;

  /// Construct an inline plain socket transport without allocating a backend.
  Transport(NativeHandle fd, ZerocopyMode zerocopyMode, uint32_t minBytesForZerocopy)
      : _storage(std::in_place_type<PlainTransport>, fd, zerocopyMode, minBytesForZerocopy) {}

  template <typename Backend>
    requires std::derived_from<Backend, ErasedTransportBackend>
  explicit Transport(std::unique_ptr<Backend> backend) noexcept {
    setBackend(std::move(backend));
  }

  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  Transport(Transport&& rhs) noexcept : _storage(std::move(rhs._storage)) { rhs.reset(); }

  Transport& operator=(Transport&& rhs) noexcept {
    if (this != &rhs) [[likely]] {
      _storage = std::move(rhs._storage);
      rhs.reset();
    }
    return *this;
  }

  template <typename Backend>
    requires std::derived_from<Backend, ErasedTransportBackend>
  Transport& operator=(std::unique_ptr<Backend> backend) noexcept {
    setBackend(std::move(backend));
    return *this;
  }

  ~Transport() = default;

  /// Create a non-owning transport view for a custom backend.
  template <typename Backend>
    requires std::derived_from<Backend, ErasedTransportBackend>
  [[nodiscard]] static Transport Borrow(Backend& backend) noexcept {
    static_assert(Backend::transportKind == TransportKind::Custom);
    Transport transport;
    transport.setBorrowedBackend(backend);
    return transport;
  }

  /// Destroy the current transport state and become empty.
  void reset() noexcept { _storage.emplace<std::monostate>(); }

  [[nodiscard]] explicit operator bool() const noexcept { return _storage.index() != kEmptyIndex; }

  [[nodiscard]] TransportKind kind() const noexcept;
  [[nodiscard]] bool isPlain() const noexcept { return _storage.index() == kPlainIndex; }
  [[nodiscard]] bool isTls() const noexcept {
    return _storage.index() == kBackendIndex && operations().kind == TransportKind::Tls;
  }

  template <typename Backend>
  [[nodiscard]] Backend* get() noexcept;

  template <typename Backend>
  [[nodiscard]] const Backend* get() const noexcept;

  // Non-blocking read. Zero bytes with no hint means orderly close.
  TransportResult read(char* buf, std::size_t len) {
    assert(*this);
    if (_storage.index() == kPlainIndex) [[likely]] {
      return plainBackend()->read(buf, len);
    }
    return operations().read(erasedBackend(), buf, len);
  }

  // Non-blocking write.
  TransportResult write(std::string_view data) {
    assert(*this);
    if (_storage.index() == kPlainIndex) [[likely]] {
      return plainBackend()->write(data);
    }
    return operations().write(erasedBackend(), data);
  }

  // Non-blocking ordered scatter write across two buffers.
  TransportResult write(std::string_view firstBuf, std::string_view secondBuf) {
    assert(*this);
    if (_storage.index() == kPlainIndex) [[likely]] {
      return plainBackend()->write(firstBuf, secondBuf);
    }
    return operations().writeTwo(erasedBackend(), firstBuf, secondBuf);
  }

  // Non-blocking ordered gather write.
  TransportResult write(std::span<const std::string_view> buffers) {
    assert(*this);
    if (_storage.index() == kPlainIndex) [[likely]] {
      return plainBackend()->write(buffers);
    }
    return operations().writeMany(erasedBackend(), buffers);
  }

  /// Whether this transport can transfer a file region straight to the socket.
  [[nodiscard]] bool supportsSendfile() const noexcept {
    assert(*this);
    return _storage.index() == kPlainIndex || operations().supportsSendfile(erasedBackend());
  }

  /// Zero-copy transfer of a file region when supportsSendfile() is true.
  TransportResult sendFile(const File& file, std::size_t& offset, std::size_t count) {
    assert(*this);
    if (_storage.index() == kPlainIndex) [[likely]] {
      return plainBackend()->sendFile(file, offset, count);
    }
    return operations().sendFile(erasedBackend(), file, offset, count);
  }

  [[nodiscard]] bool handshakeDone() const noexcept {
    assert(*this);
    return _storage.index() == kPlainIndex || operations().handshakeDone(erasedBackend());
  }

  [[nodiscard]] bool hasPendingReadData() const noexcept {
    assert(*this);
    return _storage.index() != kPlainIndex && operations().hasPendingReadData(erasedBackend());
  }

  std::size_t pollZerocopyCompletions() noexcept {
    assert(*this);
    if (_storage.index() == kPlainIndex) [[likely]] {
      return plainBackend()->pollZerocopyCompletions();
    }
    return operations().pollZerocopyCompletions(erasedBackend());
  }

  [[nodiscard]] bool isZerocopyEnabled() const noexcept {
    assert(*this);
    if (_storage.index() == kPlainIndex) [[likely]] {
      return plainBackend()->isZerocopyEnabled();
    }
    return operations().isZerocopyEnabled(erasedBackend());
  }

  // Check if there are any outstanding zerocopy sends waiting for completion.
  [[nodiscard]] bool hasZerocopyPending() const noexcept {
    assert(*this);
    if (_storage.index() == kPlainIndex) [[likely]] {
      return plainBackend()->hasZerocopyPending();
    }
    return operations().hasZerocopyPending(erasedBackend());
  }

  // Disable zerocopy for this transport (useful when buffer lifetimes are not stable,
  // e.g. CONNECT tunneling that reuses read buffers).
  void disableZerocopy() noexcept {
    assert(*this);
    if (_storage.index() == kPlainIndex) [[likely]] {
      plainBackend()->disableZerocopy();
    } else {
      operations().disableZerocopy(erasedBackend());
    }
  }

  using trivially_relocatable = std::true_type;

 private:
  struct Operations {
    TransportResult (*read)(ErasedTransportBackend*, char*, std::size_t);
    TransportResult (*write)(ErasedTransportBackend*, std::string_view);
    TransportResult (*writeTwo)(ErasedTransportBackend*, std::string_view, std::string_view);
    TransportResult (*writeMany)(ErasedTransportBackend*, std::span<const std::string_view>);
    TransportResult (*sendFile)(ErasedTransportBackend*, const File&, std::size_t&, std::size_t);
    std::size_t (*pollZerocopyCompletions)(ErasedTransportBackend*) noexcept;
    bool (*supportsSendfile)(const ErasedTransportBackend*) noexcept;
    bool (*handshakeDone)(const ErasedTransportBackend*) noexcept;
    bool (*hasPendingReadData)(const ErasedTransportBackend*) noexcept;
    bool (*isZerocopyEnabled)(const ErasedTransportBackend*) noexcept;
    bool (*hasZerocopyPending)(const ErasedTransportBackend*) noexcept;
    void (*disableZerocopy)(ErasedTransportBackend*) noexcept;
    TransportKind kind;
  };

  struct BackendDeleter {
    void operator()(ErasedTransportBackend* backend) const noexcept {
      assert(destroy != nullptr);
      destroy(backend);
    }

    void (*destroy)(ErasedTransportBackend*) noexcept {nullptr};
  };

  using BackendPtr = std::unique_ptr<ErasedTransportBackend, BackendDeleter>;
  using Storage = std::variant<std::monostate, PlainTransport, BackendPtr>;

  static_assert(std::is_nothrow_move_constructible_v<Storage>);
  static_assert(std::is_nothrow_move_assignable_v<Storage>);

  template <typename Backend, TransportKind Kind>
  friend class TransportBackend;

  template <typename Backend, TransportKind Kind>
  [[nodiscard]] static const Operations& OperationsFor() noexcept;

  static constexpr std::size_t kEmptyIndex = 0;
  static constexpr std::size_t kPlainIndex = 1;
  static constexpr std::size_t kBackendIndex = 2;

  [[nodiscard]] PlainTransport* plainBackend() noexcept { return std::get_if<PlainTransport>(&_storage); }
  [[nodiscard]] const PlainTransport* plainBackend() const noexcept { return std::get_if<PlainTransport>(&_storage); }

  [[nodiscard]] ErasedTransportBackend* erasedBackend() noexcept {
    BackendPtr* backend = std::get_if<BackendPtr>(&_storage);
    assert(backend != nullptr);
    return backend->get();
  }

  [[nodiscard]] const ErasedTransportBackend* erasedBackend() const noexcept {
    const BackendPtr* backend = std::get_if<BackendPtr>(&_storage);
    assert(backend != nullptr);
    return backend->get();
  }

  [[nodiscard]] const Operations& operations() const noexcept {
    assert(_storage.index() == kBackendIndex);
    return *static_cast<const Operations*>(erasedBackend()->_operations);
  }

  template <typename Backend>
  void setBackend(std::unique_ptr<Backend> backend) noexcept {
    assert(backend);
    static_assert(Backend::transportKind == TransportKind::Tls || Backend::transportKind == TransportKind::Custom);
    using BackendBase = TransportBackend<Backend, Backend::transportKind>;
    auto destroy = [](ErasedTransportBackend* erased) noexcept {
      std::default_delete<Backend>{}(static_cast<Backend*>(static_cast<BackendBase*>(erased)));
    };
    _storage.template emplace<BackendPtr>(static_cast<ErasedTransportBackend*>(backend.release()),
                                          BackendDeleter{destroy});
  }

  template <typename Backend>
  void setBorrowedBackend(Backend& backend) noexcept {
    auto retain = [](ErasedTransportBackend*) noexcept {};
    _storage.template emplace<BackendPtr>(static_cast<ErasedTransportBackend*>(&backend), BackendDeleter{retain});
  }

  Storage _storage;
};

/// CRTP base for TLS and custom/test transport backends.
/// Production plain sockets deliberately do not use this table and are dispatched directly by Transport.
template <typename Backend, TransportKind Kind>
class TransportBackend : public ErasedTransportBackend {
 public:
  using TransportResult = aeronet::TransportResult;
  static constexpr TransportKind transportKind = Kind;

 protected:
  TransportBackend() noexcept : ErasedTransportBackend(&Transport::OperationsFor<Backend, Kind>()) {
    static_assert(Kind == TransportKind::Tls || Kind == TransportKind::Custom);
  }

 private:
  friend class Transport;

  [[nodiscard]] static Backend& Get(ErasedTransportBackend* erased) noexcept {
    return static_cast<Backend&>(*static_cast<TransportBackend*>(erased));
  }

  [[nodiscard]] static const Backend& Get(const ErasedTransportBackend* erased) noexcept {
    return static_cast<const Backend&>(*static_cast<const TransportBackend*>(erased));
  }
};

template <typename Backend, TransportKind Kind>
const Transport::Operations& Transport::OperationsFor() noexcept {
  using BackendBase = TransportBackend<Backend, Kind>;

  static constexpr auto WriteTwo = [](ErasedTransportBackend* erased, std::string_view firstBuf,
                                      std::string_view secondBuf) -> TransportResult {
    Backend& backend = BackendBase::Get(erased);
    if constexpr (requires { backend.write(firstBuf, secondBuf); }) {
      return backend.write(firstBuf, secondBuf);
    } else {
      TransportResult result = backend.write(firstBuf);
      if (result.want != TransportHint::None || result.bytesProcessed < firstBuf.size()) {
        return result;
      }
      if (!secondBuf.empty()) {
        const auto [bytesWritten, want] = backend.write(secondBuf);
        result.bytesProcessed += bytesWritten;
        result.want = want;
      }
      return result;
    }
  };
  static constexpr auto WriteMany = [](ErasedTransportBackend* erased,
                                       std::span<const std::string_view> buffers) -> TransportResult {
    Backend& backend = BackendBase::Get(erased);
    if constexpr (requires { backend.write(buffers); }) {
      return backend.write(buffers);
    } else {
      TransportResult result{0, TransportHint::None};
      for (const std::string_view buffer : buffers) {
        if (buffer.empty()) {
          continue;
        }
        const auto [bytesWritten, want] = backend.write(buffer);
        result.bytesProcessed += bytesWritten;
        result.want = want;
        if (want != TransportHint::None || bytesWritten < buffer.size()) {
          break;
        }
      }
      return result;
    }
  };

  static const Operations operations{
      .read = [](ErasedTransportBackend* erased, char* buf,
                 std::size_t len) { return BackendBase::Get(erased).read(buf, len); },
      .write = [](ErasedTransportBackend* erased,
                  std::string_view data) { return BackendBase::Get(erased).write(data); },
      .writeTwo = WriteTwo,
      .writeMany = WriteMany,
      .sendFile = [](ErasedTransportBackend* erased, const File& file, std::size_t& offset,
                     std::size_t count) -> TransportResult {
        Backend& backend = BackendBase::Get(erased);
        if constexpr (requires { backend.sendFile(file, offset, count); }) {
          return backend.sendFile(file, offset, count);
        } else {
          return {0, TransportHint::Error};
        }
      },
      .pollZerocopyCompletions = [](ErasedTransportBackend* erased) noexcept -> std::size_t {
        Backend& backend = BackendBase::Get(erased);
        if constexpr (requires { backend.pollZerocopyCompletions(); }) {
          return backend.pollZerocopyCompletions();
        } else {
          return 0;
        }
      },
      .supportsSendfile =
          [](const ErasedTransportBackend* erased) noexcept {
            const Backend& backend = BackendBase::Get(erased);
            if constexpr (requires { backend.supportsSendfile(); }) {
              return backend.supportsSendfile();
            } else {
              return false;
            }
          },
      .handshakeDone =
          [](const ErasedTransportBackend* erased) noexcept {
            const Backend& backend = BackendBase::Get(erased);
            if constexpr (requires { backend.handshakeDone(); }) {
              return backend.handshakeDone();
            } else {
              return true;
            }
          },
      .hasPendingReadData =
          [](const ErasedTransportBackend* erased) noexcept {
            const Backend& backend = BackendBase::Get(erased);
            if constexpr (requires { backend.hasPendingReadData(); }) {
              return backend.hasPendingReadData();
            } else {
              return false;
            }
          },
      .isZerocopyEnabled =
          [](const ErasedTransportBackend* erased) noexcept {
            const Backend& backend = BackendBase::Get(erased);
            if constexpr (requires { backend.isZerocopyEnabled(); }) {
              return backend.isZerocopyEnabled();
            } else {
              return false;
            }
          },
      .hasZerocopyPending =
          [](const ErasedTransportBackend* erased) noexcept {
            const Backend& backend = BackendBase::Get(erased);
            if constexpr (requires { backend.hasZerocopyPending(); }) {
              return backend.hasZerocopyPending();
            } else {
              return false;
            }
          },
      .disableZerocopy =
          [](ErasedTransportBackend* erased) noexcept {
            Backend& backend = BackendBase::Get(erased);
            if constexpr (requires { backend.disableZerocopy(); }) {
              backend.disableZerocopy();
            }
          },
      .kind = Kind,
  };
  return operations;
}

template <typename Backend>
Backend* Transport::get() noexcept {
  if constexpr (std::is_same_v<Backend, PlainTransport>) {
    return isPlain() ? plainBackend() : nullptr;
  } else {
    if (_storage.index() != kBackendIndex || operations().kind != Backend::transportKind) {
      return nullptr;
    }
    if (erasedBackend()->_operations != &OperationsFor<Backend, Backend::transportKind>()) {
      return nullptr;
    }
    using BackendBase = TransportBackend<Backend, Backend::transportKind>;
    return static_cast<Backend*>(static_cast<BackendBase*>(erasedBackend()));
  }
}

template <typename Backend>
const Backend* Transport::get() const noexcept {
  return const_cast<Transport*>(this)->get<Backend>();
}

#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(Transport) ==
              ((sizeof(NativeHandle) == sizeof(void*)) ? 4UL * sizeof(void*) : 3UL * sizeof(void*)));
#endif

}  // namespace aeronet
