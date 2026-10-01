#ifndef GUFO_CORE_PLATFORM_FATAL_SIGNAL_HPP_
#define GUFO_CORE_PLATFORM_FATAL_SIGNAL_HPP_

// Prints one async-signal-safe line naming the fatal signal (or Win32
// exception) before the process dies with the default action, so a crash
// during model loading still leaves a diagnosable log. On Windows this also
// installs an unhandled-exception filter, because a driver fault there kills
// the process without ever raising the POSIX signals.
//
// Lives here rather than in the CLI so the Win32 headers stay at file scope;
// <windows.h> must never be included inside a namespace.

namespace gufo::platform {

void InstallFatalSignalReporter();

}  // namespace gufo::platform

#endif  // GUFO_CORE_PLATFORM_FATAL_SIGNAL_HPP_
