#include "src/core/platform/cancellable_write.hpp"

#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using gufo::platform::CancellableWrite;
using gufo::platform::PreparePipeForCancellableWrite;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

constexpr std::size_t kPipeCapacityBytes = 1U << 20U;

void TestDrainedWrite() {
  int stream[2] = {-1, -1};
  Expect(pipe(stream) == 0, "pipe() must succeed");
  std::string error;
  Expect(PreparePipeForCancellableWrite(stream[1], &error),
         "prepare must succeed");
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    std::vector<char> buffer(4096);
    while (!stop.load(std::memory_order_acquire)) {
      const ssize_t amount = ::read(stream[0], buffer.data(), buffer.size());
      if (amount <= 0) {
        break;
      }
    }
  });
  const std::vector<char> payload(64 * 1024, 'x');
  const ssize_t written =
      CancellableWrite(stream[1], payload.data(), payload.size(),
                       [&] { return stop.load(std::memory_order_acquire); });
  Expect(written == static_cast<ssize_t>(payload.size()),
         "a drained pipe must accept the whole payload");
  stop.store(true, std::memory_order_release);
  // Closing the write end delivers EOF and releases the reader's blocking
  // read(); joining first would wait on a read() nothing can wake.
  (void)close(stream[1]);
  reader.join();
  (void)close(stream[0]);
}

void TestAlreadyCancelled() {
  int stream[2] = {-1, -1};
  Expect(pipe(stream) == 0, "pipe() must succeed");
  std::string error;
  Expect(PreparePipeForCancellableWrite(stream[1], &error),
         "prepare must succeed");
  const std::vector<char> payload(1024, 'x');
  const ssize_t written = CancellableWrite(stream[1], payload.data(),
                                           payload.size(), [] { return true; });
  Expect(written == -1 && errno == ECANCELED,
         "an already-cancelled write must return ECANCELED");
  (void)close(stream[1]);
  (void)close(stream[0]);
}

void TestCancelOnFullPipe() {
  int stream[2] = {-1, -1};
  Expect(pipe(stream) == 0, "pipe() must succeed");
  std::string error;
  Expect(PreparePipeForCancellableWrite(stream[1], &error),
         "prepare must succeed");
  // Nobody reads, so a payload larger than the pipe buffer blocks the
  // writer; the predicate must still be honoured promptly.
  std::atomic<bool> cancel{false};
  std::atomic<bool> returned{false};
  std::atomic<ssize_t> result{0};
  std::atomic<int> failure_errno{0};
  std::thread writer([&] {
    const std::vector<char> payload(4U * kPipeCapacityBytes, 'x');
    result.store(CancellableWrite(stream[1], payload.data(), payload.size(),
                                  [&] { return cancel.load(); }),
                 std::memory_order_release);
    failure_errno.store(errno, std::memory_order_release);
    returned.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto start = std::chrono::steady_clock::now();
  cancel.store(true);
  while (!returned.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const bool prompt = returned.load(std::memory_order_acquire);
  // Fail before joining, so a stuck writer reports a failure instead of
  // hanging the process forever in join().
  Expect(prompt, "a full pipe write must unblock after cancellation");
  writer.join();
  Expect(result.load() == -1 && failure_errno.load() == ECANCELED,
         "the cancelled write must report ECANCELED");
  (void)close(stream[1]);
  (void)close(stream[0]);
}

}  // namespace

int main() {
  TestDrainedWrite();
  TestAlreadyCancelled();
  TestCancelOnFullPipe();
  std::cout << "cancellable_write_test: all checks passed\n";
  return 0;
}
