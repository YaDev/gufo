#ifndef GUFO_CORE_PLATFORM_CANCELLABLE_WRITE_HPP_
#define GUFO_CORE_PLATFORM_CANCELLABLE_WRITE_HPP_

// Writes to a POSIX pipe write end with responsive cancellation.
//
// On Linux the descriptor is made O_NONBLOCK by Prepare and CancellableWrite
// loops over write/EAGAIN/poll. Windows anonymous pipes have no non-blocking
// mode - CreatePipe only yields synchronous handles and fcntl(F_SETFL) fails
// with ENOTSUP rather than pretend - so the Windows arm runs a blocking
// WriteFile while a watchdog polls the cancellation predicate and breaks the
// pending transfer with CancelIoEx. Both arms present identical semantics:
// a partial-write return like POSIX write(), or -1 with errno ECANCELED when
// the predicate stopped the transfer.
//
// A descriptor must have at most one concurrent CancellableWrite.

#include <sys/types.h>

#include <cstddef>
#include <functional>
#include <string>

namespace gufo::platform {

// On failure, sets *error to the reason and returns false.
bool PreparePipeForCancellableWrite(int descriptor, std::string* error);

// Writes up to `bytes`; returns the number transferred (possibly partial) or
// -1 with errno set. ECANCELED means the predicate aborted the transfer; any
// bytes that already reached the pipe are reported by an earlier partial
// return, never silently lost or repeated.
ssize_t CancellableWrite(int descriptor, const void* data, std::size_t bytes,
                         const std::function<bool()>& cancelled);

}  // namespace gufo::platform

#endif  // GUFO_CORE_PLATFORM_CANCELLABLE_WRITE_HPP_
