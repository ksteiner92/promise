#ifndef UTIL_CONCEPTS_H_
#define UTIL_CONCEPTS_H_

#include <concepts>
#include <optional>

namespace ks {
namespace internal {

template <class T>
struct is_optional : std::false_type {};
template <class T>
struct is_optional<std::optional<T>> : std::true_type {};

}

template <class T>
concept Optional = internal::is_optional<T>::value;

template <typename T>
concept Fields = requires(T v) {
  { v.tie() } -> std::convertible_to<typename T::TieType>;
};

} // namespace ks

#endif // UTIL_CONCEPTS_H_
