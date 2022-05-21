#include "semaphore.h"

namespace ks {

void Semaphore::post() noexcept {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    ++count_;
  }
  cv_.notify_one();
}

void Semaphore::wait() noexcept {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this]() { return count_ != 0; });
  --count_;
}

}  // namespace ks
