#ifndef GUFO_CORE_PLATFORM_SOCKET_HPP_
#define GUFO_CORE_PLATFORM_SOCKET_HPP_

// The few socket operations whose POSIX spelling does not work on Winsock.
// Sockets stay `int` descriptors everywhere else; on Windows the value is the
// SOCKET handle, which always fits in 32 bits.

#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>

#ifndef _WIN32
#include <poll.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace gufo::platform {

#ifdef _WIN32
namespace detail {
/// Maps the last Winsock error onto errno so POSIX error checks keep working.
inline void SetErrnoFromSocketError() noexcept {
  switch (::WSAGetLastError()) {
    case WSAEWOULDBLOCK:
    case WSAETIMEDOUT:  // SO_RCVTIMEO/SO_SNDTIMEO expiry is EAGAIN on Linux.
      errno = EAGAIN;
      break;
    case WSAEINTR:
      errno = EINTR;
      break;
    case WSA_OPERATION_ABORTED:  // StopSocketReads cancelled the recv.
      errno = ECANCELED;
      break;
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAENETRESET:
      errno = ECONNRESET;
      break;
    case WSAESHUTDOWN:
    case WSAENOTCONN:
      errno = EPIPE;
      break;
    case WSAENOTSOCK:
      errno = EBADF;
      break;
    case WSAEADDRINUSE:
      errno = EADDRINUSE;
      break;
    case WSAEACCES:
      errno = EACCES;
      break;
    default:
      errno = EIO;
      break;
  }
}

inline int ClampLength(std::size_t size) noexcept {
  return size > static_cast<std::size_t>(INT_MAX) ? INT_MAX
                                                  : static_cast<int>(size);
}
}  // namespace detail
#endif

inline int CloseSocket(int fd) noexcept {
#ifdef _WIN32
  return ::closesocket(static_cast<SOCKET>(fd));
#else
  return ::close(fd);
#endif
}

/// Waits up to `timeout` for `fd` to become readable (or closed/errored).
inline bool WaitReadable(int fd, std::chrono::milliseconds timeout) noexcept {
#ifdef _WIN32
  WSAPOLLFD readable{static_cast<SOCKET>(fd), POLLRDNORM, 0};
  return ::WSAPoll(&readable, 1, static_cast<INT>(timeout.count())) != 0;
#else
  pollfd readable{fd, POLLIN, 0};
  return ::poll(&readable, 1, static_cast<int>(timeout.count())) != 0;
#endif
}

/// Closes an accepted connection. Closing with unread input sends a TCP
/// reset on both platforms, but Linux still lets the peer read what arrived
/// before it while Windows discards it, losing a final response or close
/// frame. On Windows, finish sending, drain briefly, then close. The drain
/// is bounded by wall clock, not by a round counter, so a chatty peer cannot
/// stretch a teardown; callers still must not hold a teardown mutex across
/// this call.
inline int CloseConnection(int fd) noexcept {
#ifdef _WIN32
  const auto socket = static_cast<SOCKET>(fd);
  (void)::shutdown(socket, SD_SEND);
  constexpr auto kDrainBudget = std::chrono::milliseconds(200);
  const auto deadline = std::chrono::steady_clock::now() + kDrainBudget;
  char sink[4096];
  while (std::chrono::steady_clock::now() < deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (!WaitReadable(fd, remaining))
      break;  // Quiet for the whole budget: the peer has what we sent.
    const int n = ::recv(socket, sink, sizeof(sink), 0);
    if (n <= 0)
      break;  // Orderly close, reset or error: nothing more to preserve.
  }
  return ::closesocket(socket);
#else
  return ::close(fd);
#endif
}

/// Wakes a thread blocked reading `fd` and stops further reads, keeping the
/// send half usable. Linux does this with shutdown(SHUT_RD). Winsock resets
/// the connection when data arrives after SD_RECEIVE, which would discard
/// frames the peer has not read yet, and neither shutdown() nor CancelIoEx
/// reliably wakes a blocking recv(); every Windows reader therefore waits
/// in short WaitReadable slices and re-checks its own stop flag, so this is
/// a no-op.
inline void StopSocketReads([[maybe_unused]] int fd) noexcept {
#ifndef _WIN32
  (void)::shutdown(fd, SHUT_RD);
#endif
}

/// read(2) on a connected socket.
inline ssize_t SocketRead(int fd, void* buffer, std::size_t size) noexcept {
#ifdef _WIN32
  const int n = ::recv(static_cast<SOCKET>(fd), static_cast<char*>(buffer),
                       detail::ClampLength(size), 0);
  if (n < 0)
    detail::SetErrnoFromSocketError();
  return n;
#else
  return ::read(fd, buffer, size);
#endif
}

inline ssize_t SocketRecv(int fd, void* buffer, std::size_t size,
                          int flags) noexcept {
#ifdef _WIN32
  const int n = ::recv(static_cast<SOCKET>(fd), static_cast<char*>(buffer),
                       detail::ClampLength(size), flags);
  if (n < 0)
    detail::SetErrnoFromSocketError();
  return n;
#else
  return ::recv(fd, buffer, size, flags);
#endif
}

inline ssize_t SocketSend(int fd, const void* data, std::size_t size,
                          int flags) noexcept {
#ifdef _WIN32
  const int n = ::send(static_cast<SOCKET>(fd), static_cast<const char*>(data),
                       detail::ClampLength(size), flags);
  if (n < 0)
    detail::SetErrnoFromSocketError();
  return n;
#else
  return ::send(fd, data, size, flags);
#endif
}

template<typename T>
int SetSocketOption(int fd, int level, int name, const T& value) noexcept {
#ifdef _WIN32
  return ::setsockopt(static_cast<SOCKET>(fd), level, name,
                      reinterpret_cast<const char*>(&value),
                      static_cast<int>(sizeof(value)));
#else
  return ::setsockopt(fd, level, name, &value, sizeof(value));
#endif
}

enum class ListenerBind {
  kBound,
  kBusy,    // The port is held by a live listener or a TIME_WAIT socket.
  kFailed,  // errno carries the reason.
};

/// Sets the reuse option and binds a listening socket.
/// Linux: SO_REUSEADDR, so an immediate restart succeeds while closed
/// connections of the previous instance are still TIME_WAIT.
/// Windows: SO_REUSEADDR means something different there - it lets another
/// process hijack a bound port - so bind exclusively first; if an old
/// TIME_WAIT connection blocks the rebind, drop back to reuse, which still
/// refuses the port from a live listener. setsockopt results are not checked
/// here; the bind result reports every condition callers can act on.
inline ListenerBind BindListener(int fd, const sockaddr& address) noexcept {
#ifdef _WIN32
  const SOCKET socket = static_cast<SOCKET>(fd);
  const int exclusive = 1;
  (void)SetSocketOption(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, exclusive);
  if (::bind(socket, &address, static_cast<int>(sizeof(address))) == 0)
    return ListenerBind::kBound;
  if (::WSAGetLastError() != WSAEADDRINUSE) {
    detail::SetErrnoFromSocketError();
    return ListenerBind::kFailed;
  }
  const int off = 0;
  const int on = 1;
  (void)SetSocketOption(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, off);
  (void)SetSocketOption(fd, SOL_SOCKET, SO_REUSEADDR, on);
  if (::bind(socket, &address, static_cast<int>(sizeof(address))) == 0)
    return ListenerBind::kBound;
  detail::SetErrnoFromSocketError();
  return errno == EADDRINUSE ? ListenerBind::kBusy : ListenerBind::kFailed;
#else
  const int on = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (::bind(fd, &address, sizeof(address)) == 0)
    return ListenerBind::kBound;
  return errno == EADDRINUSE ? ListenerBind::kBusy : ListenerBind::kFailed;
#endif
}

enum class Peek {
  kByte,        // *byte received the buffered byte without consuming it.
  kPeerClosed,  // FIN with the read side drained, or the socket died.
  kNoData,      // Connected with nothing buffered.
  kError,       // errno carries the reason.
};

/// Peeks one byte without consuming it and never blocks, even on a socket
/// in blocking mode. Linux has recv(MSG_PEEK|MSG_DONTWAIT). Winsock has no
/// MSG_DONTWAIT, and toggling FIONBIO on the connection would race with the
/// connection thread's own blocking reads, so peek through a duplicate
/// handle: Windows keeps blocking mode per handle, not per connection.
inline Peek PeekByteNonBlocking(int fd, void* byte) noexcept {
#ifdef _WIN32
  WSAPROTOCOL_INFOW info{};
  if (::WSADuplicateSocketW(static_cast<SOCKET>(fd),
                            static_cast<DWORD>(getpid()), &info) != 0) {
    detail::SetErrnoFromSocketError();
    return Peek::kError;
  }
  const SOCKET probe = ::WSASocketW(info.iAddressFamily, info.iSocketType,
                                    info.iProtocol, &info, 0, 0);
  if (probe == INVALID_SOCKET) {
    detail::SetErrnoFromSocketError();
    return Peek::kError;
  }
  u_long non_blocking = 1;
  (void)::ioctlsocket(probe, FIONBIO, &non_blocking);
  const int n = ::recv(probe, static_cast<char*>(byte), 1, MSG_PEEK);
  const int error = ::WSAGetLastError();
  (void)::closesocket(probe);
  if (n == 1)
    return Peek::kByte;
  if (n == 0)
    return Peek::kPeerClosed;
  if (error == WSAEWOULDBLOCK)
    return Peek::kNoData;
  detail::SetErrnoFromSocketError();
  return Peek::kError;
#else
  const ssize_t n = ::recv(fd, byte, 1, MSG_PEEK | MSG_DONTWAIT);
  if (n == 1)
    return Peek::kByte;
  if (n == 0)
    return Peek::kPeerClosed;
  if (errno == EAGAIN || errno == EWOULDBLOCK)
    return Peek::kNoData;
  return Peek::kError;
#endif
}

/// True without waiting when data arrived or the peer closed or reset the
/// connection. WSAPoll cannot reliably report POLLHUP for an aborted
/// connection, but it marks such sockets readable and the peek decides.
inline bool SocketReadableOrClosed(int fd) noexcept {
#ifdef _WIN32
  WSAPOLLFD readable{static_cast<SOCKET>(fd), POLLRDNORM, 0};
  return ::WSAPoll(&readable, 1, 0) > 0;
#else
  pollfd descriptor{
      .fd = fd,
      .events = POLLIN | POLLERR | POLLHUP,
      .revents = 0,
  };
#ifdef POLLRDHUP
  descriptor.events |= POLLRDHUP;
#endif
  return ::poll(&descriptor, 1, 0) > 0;
#endif
}

/// SO_RCVTIMEO / SO_SNDTIMEO. Winsock takes a DWORD of milliseconds where
/// Linux takes a timeval; passing a timeval on Windows silently sets
/// tv_sec milliseconds.
inline int SetSocketTimeout(int fd, int name,
                            std::chrono::milliseconds timeout) noexcept {
#ifdef _WIN32
  const DWORD ms = static_cast<DWORD>(timeout.count());
  return SetSocketOption(fd, SOL_SOCKET, name, ms);
#else
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(timeout);
  const timeval tv{static_cast<decltype(tv.tv_sec)>(seconds.count()),
                   static_cast<decltype(tv.tv_usec)>(
                       std::chrono::duration_cast<std::chrono::microseconds>(
                           timeout - seconds)
                           .count())};
  return SetSocketOption(fd, SOL_SOCKET, name, tv);
#endif
}

}  // namespace gufo::platform

#endif  // GUFO_CORE_PLATFORM_SOCKET_HPP_
