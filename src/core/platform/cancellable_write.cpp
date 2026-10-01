#include "src/core/platform/cancellable_write.hpp"

#include <errno.h>

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <io.h>
#include <windows.h>

#include <atomic>
#include <thread>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace gufo::platform {
namespace {

[[nodiscard]] std::string StrError() {
  return std::string(std::strerror(errno));
}

}  // namespace

#ifdef _WIN32

bool PreparePipeForCancellableWrite(int descriptor, std::string* error) {
  // Windows anonymous pipes stay blocking; CancellableWrite breaks the
  // pending WriteFile with CancelIoEx instead of relying on O_NONBLOCK.
  if (reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)) ==
      INVALID_HANDLE_VALUE) {
    errno = EBADF;
    if (error != nullptr) {
      *error = StrError();
    }
    return false;
  }
  return true;
}

ssize_t CancellableWrite(int descriptor, const void* data, std::size_t bytes,
                         const std::function<bool()>& cancelled) {
  if (cancelled()) {
    errno = ECANCELED;
    return -1;
  }
  const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
  if (handle == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  // WriteFile takes a DWORD; a smaller request with a partial-style return is
  // legal POSIX behaviour and the caller resumes at the returned offset.
  const DWORD requested = bytes > static_cast<std::size_t>(1U << 30)
                              ? static_cast<DWORD>(1U << 30)
                              : static_cast<DWORD>(bytes);

  // A full pipe blocks WriteFile indefinitely and no wait covers it, so a
  // watchdog polls the cancellation predicate and cancels the pending
  // transfer. It keeps re-arming while the predicate holds so a CancelIoEx
  // that lands before WriteFile has started is not swallowed. Cancelled
  // transfers complete with ERROR_OPERATION_ABORTED, and a write that raced
  // the cancel to completion may still report success; both are reported.
  std::atomic<bool> finished{false};
  std::thread watchdog([&] {
    while (!finished.load(std::memory_order_acquire)) {
      if (cancelled()) {
        (void)CancelIoEx(handle, nullptr);
      }
      Sleep(20);
    }
  });

  DWORD written = 0;
  const BOOL ok = WriteFile(handle, data, requested, &written, nullptr);
  const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  finished.store(true, std::memory_order_release);
  watchdog.join();

  if (ok) {
    return static_cast<ssize_t>(written);
  }
  switch (error) {
    case ERROR_OPERATION_ABORTED:
      errno = ECANCELED;
      break;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
    case ERROR_PIPE_NOT_CONNECTED:
      errno = EPIPE;
      break;
    case ERROR_INVALID_HANDLE:
      errno = EBADF;
      break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
      errno = ENOMEM;
      break;
    default:
      errno = EIO;
      break;
  }
  return -1;
}

#else

bool PreparePipeForCancellableWrite(int descriptor, std::string* error) {
  const int flags = ::fcntl(descriptor, F_GETFL);
  if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
    if (error != nullptr) {
      *error = StrError();
    }
    return false;
  }
  return true;
}

ssize_t CancellableWrite(int descriptor, const void* data, std::size_t bytes,
                         const std::function<bool()>& cancelled) {
  while (!cancelled()) {
    const ssize_t amount = ::write(descriptor, data, bytes);
    if (amount >= 0) {
      return amount;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return -1;
    }
    struct pollfd readiness{descriptor, POLLOUT, 0};
    const int result = ::poll(&readiness, 1, 50);
    if (result < 0 && errno != EINTR) {
      return -1;
    }
    if (result > 0 && (readiness.revents & (POLLERR | POLLHUP | POLLNVAL))) {
      errno = EPIPE;
      return -1;
    }
  }
  errno = ECANCELED;
  return -1;
}

#endif

}  // namespace gufo::platform
