#ifndef UTIL_ERROR_H_
#define UTIL_ERROR_H_

#include "arena.h"
#include "slice.h"
#include <exception>

namespace ks {

//#define CHK(condition, __VA_ARGS__)    \
//  if (!condition) [[unlikely]] {       \
//    throw std::runtime_error(message); \
//  }

namespace internal {

template <typename T>
concept Size = requires(T v) {
  { v.size() } -> std::convertible_to<size_t>;
};

template <typename T>
concept CharArray = std::is_same_v < std::decay_t<T>,
const char* > ;

template <typename T>
concept Data = requires(T v) {
  { v.data() } -> std::convertible_to<char*>;
};

template <typename T>
concept Copyable = (Size<T> && Data<T>) || CharArray<T>;

template <typename T>
//requires (Size<T> || CharArray<T>)
constexpr size_t get_size(T&& arg) {
  if constexpr (Size<T>) {
    return arg.size();
  } else {
    return strlen(arg);
  }
}

template <Size... T>
constexpr size_t get_size(T&&... args) {
  return (get_size(std::forward<T>(args)) + ...);
}

template <typename T>
//requires (Data<T> || CharArray<T>)
constexpr const char* get_data(T&& arg) {
  if constexpr (Data<T>) {
    return arg.data();
  } else {
    return arg;
  }
}

template <typename T>
char* copy(char* ptr, T&& arg) {
  size_t size = get_size(std::forward<T>(arg));
  memcpy(ptr, get_data(std::forward<T>(arg)), size);
  return ptr + size;
}

}  // namespace internal

template <typename... Ts>
Slice ArenaCat(Arena* arena, Ts&&... args) {
  size_t size = internal::get_size(std::forward<Ts>(args)...);
  char* buffer = arena->Allocate(size);
  char* ptr = buffer;
  ((ptr = internal::copy<Ts>(ptr, std::forward<Ts>(args))), ...);
  return Slice(buffer, size);
}

template <typename... Ts>
void CHK(bool condition, Ts&&... args) {
  if (!condition) [[unlikely]] {
    if constexpr (sizeof...(Ts)) {
      Arena arena;
      Slice message = ArenaCat(&arena, std::forward<Ts>(args)...);
      throw std::runtime_error(message.ToString());
    }
  }
}

template<typename T>
requires std::is_integral_v<T>
void SliceToInt(Slice s, T* val) {


}

}  // namespace ks

#endif  // UTIL_ERROR_H_
