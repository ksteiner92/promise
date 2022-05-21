#ifndef UTIL_SEMAPHORE_H_
#define UTIL_SEMAPHORE_H_

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace ks {

class Semaphore {
 public:
  explicit Semaphore(size_t count = 0) noexcept : count_(count) {}

  void post() noexcept;

  void wait() noexcept;

 private:
  size_t count_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

}  // namespace ks

#endif  // UTIL_SEMAPHORE_H_
