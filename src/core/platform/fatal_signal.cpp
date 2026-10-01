#include "src/core/platform/fatal_signal.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <signal.h>
#include <unistd.h>

#include <csignal>
#include <cstddef>

namespace gufo::platform {
namespace {

// Reports a fatal signal on the way out. A driver or runtime failure during a
// load can abort the process outright, which runs no destructor and no
// terminate handler, leaving a log that stops at event=load_started. Only
// async-signal-safe calls are allowed here, so the line is assembled by hand
// and written straight to the descriptor.
void ReportFatalSignal(int number) {
  static constexpr char kPrefix[] =
      "[ERROR] [server] event=fatal_signal signal=";
  char digits[8];
  std::size_t length = 0;
  int value = number;
  if (value <= 0) {
    digits[length++] = '0';
  } else {
    char reversed[8];
    std::size_t count = 0;
    while (value > 0 && count < sizeof(reversed)) {
      reversed[count++] = static_cast<char>('0' + (value % 10));
      value /= 10;
    }
    while (count > 0) {
      digits[length++] = reversed[--count];
    }
  }
  (void)::write(STDERR_FILENO, kPrefix, sizeof(kPrefix) - 1);
  (void)::write(STDERR_FILENO, digits, length);
  (void)::write(STDERR_FILENO, "\n", 1);
  // Restore the default action and re-raise, so the exit status and any core
  // dump still describe the original fault.
  struct sigaction restore{};
  restore.sa_handler = SIG_DFL;
  (void)::sigemptyset(&restore.sa_mask);
  (void)::sigaction(number, &restore, nullptr);
  (void)::raise(number);
}

#ifdef _WIN32

LONG WINAPI ReportUnhandledException(EXCEPTION_POINTERS* exception_info) {
  if (exception_info == nullptr || exception_info->ExceptionRecord == nullptr) {
    ReportFatalSignal(SIGABRT);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  const DWORD code = exception_info->ExceptionRecord->ExceptionCode;

  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_STACK_OVERFLOW:
      ReportFatalSignal(SIGSEGV);
      break;

    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
      ReportFatalSignal(SIGILL);
      break;

    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
      ReportFatalSignal(SIGFPE);
      break;

    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
      ReportFatalSignal(SIGABRT);
      break;

    default:
      ReportFatalSignal(SIGABRT);
      break;
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallFatalSignalReporter() {
  (void)std::signal(SIGABRT, ReportFatalSignal);
  (void)std::signal(SIGSEGV, ReportFatalSignal);
  (void)std::signal(SIGILL, ReportFatalSignal);
  (void)std::signal(SIGFPE, ReportFatalSignal);

  (void)::SetUnhandledExceptionFilter(ReportUnhandledException);
}

#else

}  // namespace

void InstallFatalSignalReporter() {
  struct sigaction action{};
  action.sa_handler = ReportFatalSignal;
  (void)::sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESETHAND;
  for (const int number : {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE}) {
    (void)::sigaction(number, &action, nullptr);
  }
}

#endif

}  // namespace gufo::platform
