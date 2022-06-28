#ifndef UTIL_ARENA_H_
#define UTIL_ARENA_H_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ks {

class Arena {
 public:
  Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  ~Arena();

  // Return a pointer to a newly allocated memory block of "bytes" bytes.
  char* Allocate(size_t bytes);

  // Allocate memory with the normal alignment guarantees provided by malloc.
  char* AllocateAligned(size_t bytes);

  // Returns an estimate of the total memory usage of data allocated
  // by the arena.
  size_t MemoryUsage() const {
    return memory_usage_;
  }

  template<typename T>
  T* New() {
    char* mem = Allocate(sizeof(T));
    return new (mem) T{};
  }


  template <typename T, typename... Args>
  requires (!std::is_array_v<T>)
  T* New(Args... args) {
    return new (Allocate(sizeof(T))) T(std::forward<Args>(args)...);
  }

  template <typename T, typename Base = std::decay_t<decltype(std::declval<T>()[0])>>
  requires std::is_array_v<T>
  Base* New() {
    return new (Allocate(sizeof(T))) Base[sizeof(T)/sizeof(Base)];
  }

 private:
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  // Allocation state
  char* alloc_ptr_;
  size_t alloc_bytes_remaining_;

  // Array of new[] allocated memory blocks
  std::vector<std::unique_ptr<char[]>> blocks_;

  // Total memory usage of the arena.
  size_t memory_usage_;
};

inline char* Arena::Allocate(size_t bytes) {
  // The semantics of what to return are a bit messy if we allow
  // 0-byte allocations, so we disallow them here (we don't need
  // them for our internal use).
  assert(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

}  // namespace ks

#endif  // UTIL_ARENA_H_
